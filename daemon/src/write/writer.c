/*
 * write/writer.c: Managed upsert-by-id writer, delete and extend operations, and unsigned prepare.
 */
#include "write/writer.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "postgres.h"
#include "varatt.h"
#include "vector.h"

#include "arkiv/abi.h"
#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "arkiv/logs.h"
#include "arkiv/tx.h"
#include "codec/base64.h"
#include "codec/hex.h"
#include "codec/text.h"
#include "codec/vectorbytes.h"
#include "codec/vtype.h"
#include "core/coldef.h"
#include "filter/value.h"
#include "server/metrics.h"
#include "store/schema.h"

/*
 * Fallback gas when eth_estimateGas is unavailable. One create of a small halfvec entity used about
 * 135k gas on Rabdi (21k intrinsic + calldata + storage); 150k per op plus a 60k base covers it
 * without the old 500k-per-transaction over-reservation (gas limit x max fee must be affordable).
 */
#define AV_GAS_PER_OP_FALLBACK 150000ULL
#define AV_GAS_BASE_FALLBACK   60000ULL

struct av_writer {
    av_writer_config cfg;
    av_signer *signer;
    av_rpc *rpc;
    av_store *store;
    av_registry *reg;
    av_nonce_tracker *nonce_tracker;
    /* submit_mu: entity-nonce reservation + tx nonce allocation + broadcast (see writer.h) */
    pthread_mutex_t submit_mu;
    uint64_t entity_nonce_floor; /* next entityNonce after this daemon's own in-flight creates */
    bool entity_nonce_valid;
};

av_writer *av_writer_create(const av_writer_config *cfg, av_signer *signer, av_rpc *rpc, av_store *store,
                            av_registry *reg, av_err *e)
{
    (void) e;
    av_writer *w = calloc(1, sizeof(*w));
    if (w == NULL)
        return NULL;
    if (cfg != NULL)
        w->cfg = *cfg;
    if (w->cfg.chain_id == 0)
        w->cfg.chain_id = 7794095;
    if (memcmp(w->cfg.contract_addr, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 0) {
        av_addr_from_hex(AV_ARKIV_CONTRACT_ADDR, w->cfg.contract_addr);
    }
    if (w->cfg.gas_limit == 0)
        w->cfg.gas_limit = AV_GAS_BASE_FALLBACK;
    if (w->cfg.max_fee_per_gas == 0)
        w->cfg.max_fee_per_gas = 20000000000ULL; /* 20 gwei */
    if (w->cfg.max_priority_fee_per_gas == 0)
        w->cfg.max_priority_fee_per_gas = 1500000000ULL; /* 1.5 gwei */
    if (w->cfg.priority_fee_percentile <= 0.0 || w->cfg.priority_fee_percentile > 100.0)
        w->cfg.priority_fee_percentile = 50.0;
    if (w->cfg.fee_history_blocks == 0)
        w->cfg.fee_history_blocks = 20;
    if (w->cfg.gas_margin_pct == 0)
        w->cfg.gas_margin_pct = 20;
    if (w->cfg.replace_after_blocks == 0)
        w->cfg.replace_after_blocks = 10;
    if (w->cfg.max_replacements == 0)
        w->cfg.max_replacements = 3;
    pthread_mutex_init(&w->submit_mu, NULL);

    w->signer = signer;
    w->rpc = rpc;
    w->store = store;
    w->reg = reg;

    if (signer != NULL) {
        const uint8_t *addr = av_signer_get_address(signer);
        w->nonce_tracker = av_nonce_tracker_create(addr, rpc);
    }

    return w;
}

void av_writer_destroy(av_writer *w)
{
    if (w == NULL)
        return;
    if (w->nonce_tracker != NULL) {
        av_nonce_tracker_destroy(w->nonce_tracker);
        w->nonce_tracker = NULL;
    }
    pthread_mutex_destroy(&w->submit_mu);
    free(w);
}

const av_signer *av_writer_get_signer(const av_writer *w)
{
    return w != NULL ? w->signer : NULL;
}

const uint8_t *av_writer_get_address(const av_writer *w)
{
    return (w != NULL && w->signer != NULL) ? av_signer_get_address(w->signer) : NULL;
}

/* One user attribute from JSON: {"type","value"} or a shorthand scalar typed from the schema
 * (string -> str, boolean -> bool, integer -> i32 when the schema has no entry). Values are
 * encoded as Arkiv wire bytes: 32-byte words for numeric/bool/addr/key, raw UTF-8 for str,
 * base64-decoded bytes for bytes. */
static av_status build_user_attr(const av_coldef *def, const char *name, yyjson_val *jv, av_arena *arena,
                                 av_attr *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (name == NULL || jv == NULL || strlen(name) > AV_ATTR_NAME_MAX) {
        av_err_set(e, "invalid_argument", "invalid attribute name");
        return AV_ERR;
    }
    strncpy(out->name, name, sizeof(out->name) - 1);

    av_attr_type t = AV_AT_TOMBSTONE;
    yyjson_val *vnode = jv;
    if (yyjson_is_obj(jv)) {
        const char *ts = yyjson_get_str(yyjson_obj_get(jv, "type"));
        if (ts == NULL || !av_type_from_name(ts, strlen(ts), &t)) {
            av_err_set(e, "invalid_argument", "attribute '%s': unknown type", name);
            return AV_ERR;
        }
        vnode = yyjson_obj_get(jv, "value");
    } else {
        bool in_schema = false;
        for (size_t i = 0; i < def->n_attrs && !in_schema; i++) {
            if (strcmp(def->attrs[i].name, name) == 0) {
                t = def->attrs[i].type;
                in_schema = true;
            }
        }
        if (!in_schema)
            t = yyjson_is_bool(jv) ? AV_AT_BOOL : yyjson_is_int(jv) ? AV_AT_I32 : AV_AT_STR;
    }
    out->type = (uint8_t) t;
    out->indexed = t != AV_AT_BYTES;

    if (t == AV_AT_BYTES) {
        const char *b64 = yyjson_get_str(vnode);
        uint8_t *raw = NULL;
        size_t rl = 0;
        if (b64 == NULL || av_base64_decode(b64, strlen(b64), arena, &raw, &rl, e) != AV_OK) {
            av_err_set(e, "invalid_argument", "attribute '%s': bytes must be base64", name);
            return AV_ERR;
        }
        out->val = raw;
        out->len = rl;
        return AV_OK;
    }

    av_value v;
    const char *why = "invalid value";
    bool ok = false;
    if (yyjson_is_str(vnode)) {
        const char *vs = yyjson_get_str(vnode);
        ok = av_value_parse(t, vs, strlen(vs), &v, &why);
    } else if (yyjson_is_bool(vnode) && t == AV_AT_BOOL) {
        av_value_bool(&v, yyjson_get_bool(vnode));
        ok = true;
    } else if (yyjson_is_int(vnode) &&
               (t == AV_AT_I32 || t == AV_AT_U64 || t == AV_AT_U256 || t == AV_AT_DEC)) {
        char buf[32];
        if (yyjson_is_sint(vnode))
            snprintf(buf, sizeof(buf), "%lld", (long long) yyjson_get_sint(vnode));
        else
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long) yyjson_get_uint(vnode));
        ok = av_value_parse(t, buf, strlen(buf), &v, &why);
    }
    if (!ok) {
        av_err_set(e, "invalid_argument", "attribute '%s' (%s): %s", name, av_type_name(t), why);
        return AV_ERR;
    }
    uint8_t *copy = av_arena_alloc(arena, v.len > 0 ? v.len : 1, 0);
    if (copy == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    memcpy(copy, v.b, v.len);
    out->val = copy;
    out->len = v.len;
    return AV_OK;
}

/* Parse TTL object */
static bool parse_ttl_obj(yyjson_val *ttl_val, uint64_t *expires_at, uint64_t *min_lifetime, av_err *e)
{
    *expires_at = 0;
    *min_lifetime = 0;
    if (ttl_val == NULL || !yyjson_is_obj(ttl_val))
        return true;

    yyjson_val *blocks = yyjson_obj_get(ttl_val, "blocks");
    yyjson_val *seconds = yyjson_obj_get(ttl_val, "seconds");
    yyjson_val *days = yyjson_obj_get(ttl_val, "days");
    yyjson_val *exp_block = yyjson_obj_get(ttl_val, "expires_at_block");

    int fields_set = (blocks != NULL) + (seconds != NULL) + (days != NULL) + (exp_block != NULL);
    if (fields_set != 1) {
        av_err_set(e, "invalid_argument",
                   "ttl must have exactly one of blocks, seconds, days, expires_at_block");
        return false;
    }

    if (blocks != NULL) {
        if (!yyjson_is_uint(blocks)) {
            av_err_set(e, "invalid_argument", "ttl.blocks must be a positive integer");
            return false;
        }
        *min_lifetime = yyjson_get_uint(blocks);
    } else if (seconds != NULL) {
        if (!yyjson_is_num(seconds)) {
            av_err_set(e, "invalid_argument", "ttl.seconds must be numeric");
            return false;
        }
        double s = yyjson_get_num(seconds);
        *min_lifetime = (uint64_t) round(s / 2.0);
    } else if (days != NULL) {
        if (!yyjson_is_num(days)) {
            av_err_set(e, "invalid_argument", "ttl.days must be numeric");
            return false;
        }
        double d = yyjson_get_num(days);
        *min_lifetime = (uint64_t) round(d * 43200.0);
    } else if (exp_block != NULL) {
        if (!yyjson_is_uint(exp_block)) {
            av_err_set(e, "invalid_argument", "ttl.expires_at_block must be a positive integer");
            return false;
        }
        *expires_at = yyjson_get_uint(exp_block);
    }

    if (*min_lifetime > 7884000ULL) {
        av_err_set(e, "invalid_argument", "ttl lifetime exceeds protocol maximum (7884000 blocks)");
        return false;
    }

    return true;
}

/* Collection default_ttl for creates without `ttl` (collections.md: if null, the upsert MUST carry ttl). */
static bool apply_default_ttl(const av_coldef *def, uint64_t *expires_at, uint64_t *min_lifetime, av_err *e)
{
    switch (def->default_ttl.kind) {
    case AV_TTL_BLOCKS:
        *min_lifetime = (uint64_t) def->default_ttl.value;
        return true;
    case AV_TTL_SECONDS:
        *min_lifetime = (uint64_t) (def->default_ttl.value / 2.0);
        return true;
    case AV_TTL_DAYS:
        *min_lifetime = (uint64_t) round(def->default_ttl.value * 43200.0);
        return true;
    case AV_TTL_EXPIRES_AT:
        *expires_at = (uint64_t) def->default_ttl.value;
        return true;
    default:
        av_err_set(e, "invalid_argument", "ttl is required: collection '%s' has no default_ttl", def->name);
        return false;
    }
}

bool av_writer_parse_ttl(yyjson_val *ttl_val, uint64_t *expires_at, uint64_t *min_lifetime, av_err *e)
{
    return parse_ttl_obj(ttl_val, expires_at, min_lifetime, e);
}

static av_status parse_entity_vector(yyjson_val *vec_val, yyjson_val *vb_val, const av_coldef *coldef,
                                     av_arena *arena, uint8_t **out_vecbytes, size_t *out_len, av_err *e)
{
    *out_vecbytes = NULL;
    *out_len = 0;
    if (vec_val == NULL && vb_val == NULL) {
        return AV_OK;
    }

    if (vb_val != NULL && yyjson_is_str(vb_val)) {
        const char *b64 = yyjson_get_str(vb_val);
        if (av_base64_decode(b64, strlen(b64), arena, out_vecbytes, out_len, e) != AV_OK) {
            return AV_ERR;
        }
        av_vb_info vbi;
        if (av_vb_validate(*out_vecbytes, *out_len, &vbi, e) != AV_OK) {
            return AV_ERR;
        }
        if ((int64_t) vbi.dim != coldef->dimensions) {
            av_err_set(e, "dimension_mismatch", "vector dimension %d does not match collection dimension %ld",
                       (int) vbi.dim, (long) coldef->dimensions);
            return AV_ERR;
        }
        return AV_OK;
    }

    if (vec_val != NULL && yyjson_is_arr(vec_val)) {
        size_t vdim = yyjson_arr_size(vec_val);
        if (vdim != (size_t) coldef->dimensions) {
            av_err_set(e, "dimension_mismatch",
                       "vector dimension %zu does not match collection dimension %ld", vdim,
                       (long) coldef->dimensions);
            return AV_ERR;
        }
        size_t cap = vdim * 32 + 16;
        char *buf = av_arena_alloc(arena, cap, 1);
        if (buf == NULL) {
            av_err_set(e, "internal", "out of memory allocating vector buffer");
            return AV_ERR;
        }
        size_t pos = 0;
        buf[pos++] = '[';
        for (size_t d = 0; d < vdim; d++) {
            yyjson_val *elem = yyjson_arr_get(vec_val, d);
            double val = yyjson_get_num(elem);
            int written = snprintf(buf + pos, cap - pos, "%s%.9g", (d > 0 ? "," : ""), val);
            if (written < 0 || (size_t) written >= cap - pos) {
                av_err_set(e, "internal", "buffer overflow formatting vector");
                return AV_ERR;
            }
            pos += (size_t) written;
        }
        buf[pos++] = ']';
        buf[pos] = '\0';

        void *pgvec = NULL;
        if (av_text_to_pgvec(coldef->vtype, buf, (int32_t) coldef->dimensions, arena, &pgvec, e) != AV_OK) {
            return AV_ERR;
        }
        return av_vb_encode(pgvec, coldef->vtype, false, arena, out_vecbytes, out_len, e);
    }

    if (vec_val != NULL && yyjson_is_str(vec_val)) {
        const char *txt = yyjson_get_str(vec_val);
        void *pgvec = NULL;
        if (av_text_to_pgvec(coldef->vtype, txt, (int32_t) coldef->dimensions, arena, &pgvec, e) != AV_OK) {
            return AV_ERR;
        }
        return av_vb_encode(pgvec, coldef->vtype, false, arena, out_vecbytes, out_len, e);
    }

    av_err_set(e, "invalid_argument", "vector must be an array or string");
    return AV_ERR;
}

/* ------------------------------------------------------------------ transactions */

#define AV_MAX_REPLACEMENTS 8

static av_mfamily *g_m_gas_used, *g_m_gas_reserved, *g_m_gas_price, *g_m_fee_source, *g_m_gas_fallback,
    *g_m_replacements, *g_m_txs;
static pthread_once_t g_metrics_once = PTHREAD_ONCE_INIT;

static void writer_metrics_init(void)
{
    static const char *const src_label[] = {"source"};
    g_m_gas_used = av_metric_register("arkivector_write_gas_used_total",
                                      "Gas used by mined write transactions", AV_MTYPE_COUNTER, NULL, 0);
    g_m_gas_reserved =
        av_metric_register("arkivector_write_gas_reserved_total",
                           "Gas limit reserved by mined write transactions", AV_MTYPE_COUNTER, NULL, 0);
    g_m_gas_price =
        av_metric_register("arkivector_write_effective_gas_price_wei",
                           "effectiveGasPrice of the last mined write transaction", AV_MTYPE_GAUGE, NULL, 0);
    g_m_fee_source = av_metric_register("arkivector_write_fee_quotes_total",
                                        "Fee quotes by source (fee_history, gas_price, static)",
                                        AV_MTYPE_COUNTER, src_label, 1);
    g_m_gas_fallback = av_metric_register("arkivector_write_gas_estimate_fallback_total",
                                          "Transactions whose gas limit fell back to the static estimate",
                                          AV_MTYPE_COUNTER, NULL, 0);
    g_m_replacements =
        av_metric_register("arkivector_write_tx_replacements_total",
                           "Stuck transactions resent with bumped fees", AV_MTYPE_COUNTER, NULL, 0);
    g_m_txs = av_metric_register("arkivector_write_transactions_total", "Write transactions broadcast",
                                 AV_MTYPE_COUNTER, NULL, 0);
}

static av_fee_config fee_config(const av_writer *w)
{
    av_fee_config fc;
    memset(&fc, 0, sizeof(fc));
    fc.max_fee_cap = w->cfg.max_fee_cap;
    fc.priority_percentile = w->cfg.priority_fee_percentile;
    fc.history_blocks = w->cfg.fee_history_blocks;
    fc.static_max_fee = w->cfg.max_fee_per_gas;
    fc.static_priority_fee = w->cfg.max_priority_fee_per_gas;
    av_fee_config_defaults(&fc);
    return fc;
}

static char *hex_alloc(av_arena *arena, const uint8_t *b, size_t n)
{
    static const char hexd[] = "0123456789abcdef";
    char *s = av_arena_alloc(arena, n * 2 + 3, 1);
    if (s == NULL)
        return NULL;
    s[0] = '0';
    s[1] = 'x';
    for (size_t i = 0; i < n; i++) {
        s[2 + 2 * i] = hexd[b[i] >> 4];
        s[3 + 2 * i] = hexd[b[i] & 15];
    }
    s[2 + 2 * n] = '\0';
    return s;
}

/*
 * eth_estimateGas plus gas_margin_pct. Returns the static fallback (and counts/logs it) when the call
 * fails, so a flaky RPC never blocks a write. *used_fallback tells the caller which path ran.
 */
static uint64_t estimate_gas_limit(av_writer *w, const uint8_t from[20], const uint8_t *data, size_t data_len,
                                   size_t n_ops, av_arena *arena, bool *used_fallback)
{
    (void) pthread_once(&g_metrics_once, writer_metrics_init);
    *used_fallback = false;
    const char *why = "no RPC configured";
    av_err ee;
    av_err_clear(&ee);
    if (w->rpc != NULL && from != NULL) {
        char *from_hex = hex_alloc(arena, from, 20);
        char *to_hex = hex_alloc(arena, w->cfg.contract_addr, 20);
        char *data_hex = hex_alloc(arena, data, data_len);
        size_t cap = data_len * 2 + 160;
        char *params = av_arena_alloc(arena, cap, 1);
        why = "out of memory";
        if (from_hex != NULL && to_hex != NULL && data_hex != NULL && params != NULL) {
            snprintf(params, cap, "[{\"from\":\"%s\",\"to\":\"%s\",\"data\":\"%s\"}]", from_hex, to_hex,
                     data_hex);
            yyjson_doc *doc = NULL;
            yyjson_val *res = NULL;
            int code = 0;
            why = "eth_estimateGas returned an unusable value";
            if (av_rpc_call(w->rpc, "eth_estimateGas", params, &doc, &res, &code, &ee) == AV_OK) {
                uint64_t est = 0;
                const char *hex = yyjson_get_str(res);
                bool ok = hex != NULL && av_qty_parse(hex, &est) && est > 0 && est < 30000000ULL;
                yyjson_doc_free(doc);
                if (ok)
                    return est + est * w->cfg.gas_margin_pct / 100;
            } else {
                why = ee.msg;
            }
        }
    }
    uint64_t out = w->cfg.gas_limit + (uint64_t) n_ops * AV_GAS_PER_OP_FALLBACK;
    *used_fallback = true;
    av_log(AV_LOG_WARN, "writer", "gas estimate fallback (%s): %" PRIu64 " gas for %zu ops", why, out, n_ops);
    av_metric_inc(g_m_gas_fallback, NULL, 1);
    return out;
}

/* eth_call entityNonce(owner) at "pending" on the Arkiv contract. */
static av_status fetch_entity_nonce(av_writer *w, const uint8_t owner[20], uint64_t *out, av_err *e)
{
    if (w->rpc == NULL) {
        av_err_set(e, "arkiv_rpc_error", "no Arkiv RPC configured to read entityNonce");
        return AV_ERR;
    }
    static const char hexd[] = "0123456789abcdef";
    char to_hex[43];
    av_addr_to_hex(w->cfg.contract_addr, to_hex);
    char data[2 + 8 + 64 + 1];
    memcpy(data, "0x36917bfd", 10); /* entityNonce(address) */
    memset(data + 10, '0', 24);
    for (size_t i = 0; i < 20; i++) {
        data[34 + 2 * i] = hexd[owner[i] >> 4];
        data[35 + 2 * i] = hexd[owner[i] & 15];
    }
    data[74] = '\0';
    char params[192];
    snprintf(params, sizeof(params), "[{\"to\":\"%s\",\"data\":\"%s\"},\"pending\"]", to_hex, data);
    yyjson_doc *doc = NULL;
    yyjson_val *res = NULL;
    int code = 0;
    if (av_rpc_call(w->rpc, "eth_call", params, &doc, &res, &code, e) != AV_OK)
        return AV_ERR;
    uint8_t word[32];
    const char *hex = yyjson_get_str(res);
    bool ok = hex != NULL && av_key_from_hex(hex, word);
    if (ok) {
        for (size_t i = 0; i < 24; i++)
            if (word[i] != 0)
                ok = false;
    }
    if (doc != NULL)
        yyjson_doc_free(doc);
    if (!ok) {
        av_err_set(e, "arkiv_rpc_error", "entityNonce returned a malformed value");
        return AV_ERR;
    }
    uint64_t n = 0;
    for (size_t i = 24; i < 32; i++)
        n = (n << 8) | word[i];
    *out = n;
    return AV_OK;
}

static void invalidate_nonces(av_writer *w)
{
    pthread_mutex_lock(&w->submit_mu);
    w->entity_nonce_valid = false;
    pthread_mutex_unlock(&w->submit_mu);
    av_nonce_tracker_invalidate(w->nonce_tracker);
}

/* One broadcast transaction and its replacements (same nonce, bumped fees). */
typedef struct {
    av_tx_desc desc;
    av_fee_quote fee;
    char hashes[AV_MAX_REPLACEMENTS + 1][67];
    size_t n_hashes;
    uint64_t sent_block;
    size_t first_create; /* index into the create list (keys from the receipt logs) */
    size_t n_creates;
    char mined_hash[67];
    uint64_t block;
} pending_tx;

/* Sign desc and eth_sendRawTransaction it; writes the hash. */
static av_status sign_and_send(av_writer *w, const av_tx_desc *desc, av_arena *arena, char hash_out[67],
                               av_err *e)
{
    uint8_t *raw_tx = NULL;
    size_t raw_len = 0;
    uint8_t tx_hash[32];
    if (av_tx_sign_and_encode(desc, w->signer, arena, &raw_tx, &raw_len, tx_hash, e) != AV_OK)
        return AV_ERR;
    av_key_to_hex(tx_hash, hash_out);
    if (w->rpc == NULL)
        return AV_OK;
    char *raw_hex = hex_alloc(arena, raw_tx, raw_len);
    size_t cap = raw_len * 2 + 16;
    char *params = av_arena_alloc(arena, cap, 1);
    if (raw_hex == NULL || params == NULL) {
        av_err_set(e, "internal", "out of memory for raw transaction");
        return AV_ERR;
    }
    snprintf(params, cap, "[\"%s\"]", raw_hex);
    yyjson_doc *doc = NULL;
    yyjson_val *res = NULL;
    int code = 0;
    av_status st = av_rpc_call(w->rpc, "eth_sendRawTransaction", params, &doc, &res, &code, e);
    if (doc != NULL)
        yyjson_doc_free(doc);
    if (st != AV_OK) {
        /* av_rpc_call reports JSON-RPC errors (reverts at submission, underpriced, nonce) as arkiv_rpc_error */
        if (code != 0) {
            char msg[sizeof(e->msg)];
            snprintf(msg, sizeof(msg), "%s", e->msg);
            av_err_set(e, "tx_reverted", "transaction rejected: %s", msg);
        }
        return AV_ERR;
    }
    av_metric_inc(g_m_txs, NULL, 1);
    return AV_OK;
}

typedef struct {
    av_upsert_item_result *results; /* NULL for delete/extend */
    const size_t *create_res;       /* create i -> results index */
    size_t n_creates;
    bool strict_prediction; /* wait none: a failed entityNonce read fails the request */
} key_plan;

/*
 * Reserve entity nonces, allocate transaction nonces, sign and broadcast every batch, in order,
 * under the submit mutex. Gas limits and the fee quote are computed before taking it.
 */
static av_status submit_batches(av_writer *w, const av_op_batch *batches, size_t n_batches,
                                const av_mutation_op *ops_base, key_plan *kp, av_arena *arena,
                                pending_tx **out, av_err *e)
{
    (void) pthread_once(&g_metrics_once, writer_metrics_init);
    *out = NULL;
    pending_tx *txs = av_arena_alloc(arena, n_batches * sizeof(*txs), _Alignof(pending_tx));
    if (txs == NULL) {
        av_err_set(e, "internal", "out of memory allocating transactions");
        return AV_ERR;
    }
    memset(txs, 0, n_batches * sizeof(*txs));

    av_fee_config fc = fee_config(w);
    av_fee_quote fee;
    av_fee_quote_rpc(w->rpc, &fc, &fee);
    const char *src_name = av_fee_source_name(fee.source);
    av_metric_inc(g_m_fee_source, &src_name, 1);

    const uint8_t *from = av_signer_get_address(w->signer);
    size_t create_cursor = 0;
    for (size_t i = 0; i < n_batches; i++) {
        bool fb = false;
        txs[i].desc.chain_id = w->cfg.chain_id;
        txs[i].desc.max_priority_fee_per_gas = fee.max_priority_fee_per_gas;
        txs[i].desc.max_fee_per_gas = fee.max_fee_per_gas;
        txs[i].desc.gas_limit = estimate_gas_limit(w, from, batches[i].calldata, batches[i].calldata_len,
                                                   batches[i].n_ops, arena, &fb);
        memcpy(txs[i].desc.to, w->cfg.contract_addr, 20);
        txs[i].desc.data = batches[i].calldata;
        txs[i].desc.data_len = batches[i].calldata_len;
        txs[i].fee = fee;
        txs[i].first_create = create_cursor;
        for (size_t j = 0; j < batches[i].n_ops; j++)
            if (batches[i].ops[j].type == AV_OP_CREATE)
                txs[i].n_creates++;
        create_cursor += txs[i].n_creates;
    }
    (void) ops_base;

    pthread_mutex_lock(&w->submit_mu);
    if (kp != NULL && kp->n_creates > 0) {
        uint64_t chain_nonce = 0;
        av_err ne;
        av_err_clear(&ne);
        bool have = fetch_entity_nonce(w, from, &chain_nonce, &ne) == AV_OK;
        if (!have && kp->strict_prediction) {
            pthread_mutex_unlock(&w->submit_mu);
            av_err_set(e, "arkiv_rpc_error",
                       "cannot predict entity keys: reading entityNonce failed (%s); retry, or use wait "
                       "\"receipt\" to take keys from the receipts",
                       ne.msg);
            return AV_ERR;
        }
        if (!have)
            av_log(AV_LOG_WARN, "writer", "entityNonce read failed (%s); keys will come from the receipts",
                   ne.msg);
        uint64_t base = chain_nonce;
        if (w->entity_nonce_valid && w->entity_nonce_floor > base)
            base = w->entity_nonce_floor;
        if (!have && !w->entity_nonce_valid)
            base = 0; /* placeholder: replaced from the receipt logs */
        size_t ci = 0;
        for (size_t i = 0; i < n_batches; i++) {
            for (size_t j = 0; j < batches[i].n_ops; j++) {
                const av_mutation_op *op = &batches[i].ops[j];
                if (op->type != AV_OP_CREATE)
                    continue;
                av_abi_predict_entity_key(w->cfg.chain_id, w->cfg.contract_addr, from, base + ci,
                                          op->u.create.salt, kp->results[kp->create_res[ci]].key);
                ci++;
            }
        }
        if (have || w->entity_nonce_valid) {
            w->entity_nonce_floor = base + kp->n_creates;
            w->entity_nonce_valid = true;
        }
    }
    for (size_t i = 0; i < n_batches; i++) {
        uint64_t nonce = 0;
        av_status st = av_nonce_tracker_next(w->nonce_tracker, &nonce, e);
        if (st == AV_OK) {
            txs[i].desc.nonce = nonce;
            st = sign_and_send(w, &txs[i].desc, arena, txs[i].hashes[0], e);
        }
        if (st != AV_OK) {
            w->entity_nonce_valid = false;
            pthread_mutex_unlock(&w->submit_mu);
            av_nonce_tracker_invalidate(w->nonce_tracker);
            return AV_ERR;
        }
        txs[i].n_hashes = 1;
        if (w->rpc != NULL)
            (void) av_rpc_block_number(w->rpc, &txs[i].sent_block, e);
        av_err_clear(e);
        av_log(AV_LOG_INFO, "writer",
               "tx %s nonce %" PRIu64 ": %zu ops, gas limit %" PRIu64 ", max fee %" PRIu64
               " wei, tip %" PRIu64 " wei (%s)",
               txs[i].hashes[0], nonce, batches[i].n_ops, txs[i].desc.gas_limit, txs[i].desc.max_fee_per_gas,
               txs[i].desc.max_priority_fee_per_gas, src_name);
    }
    pthread_mutex_unlock(&w->submit_mu);
    *out = txs;
    return AV_OK;
}

/* Resend tx with the same nonce and bumped fees. Errors (already mined, underpriced) are logged only. */
static void replace_tx(av_writer *w, pending_tx *tx, av_arena *arena)
{
    av_fee_config fc = fee_config(w);
    av_fee_quote fresh, bumped;
    av_fee_quote_rpc(w->rpc, &fc, &fresh);
    av_fee_bump(&tx->fee, &fresh, &fc, &bumped);
    av_tx_desc d = tx->desc;
    d.max_fee_per_gas = bumped.max_fee_per_gas;
    d.max_priority_fee_per_gas = bumped.max_priority_fee_per_gas;
    av_err e;
    av_err_clear(&e);
    char hash[67];
    if (sign_and_send(w, &d, arena, hash, &e) != AV_OK) {
        av_log(AV_LOG_WARN, "writer", "replacement of tx %s (nonce %" PRIu64 ") not accepted: %s",
               tx->hashes[tx->n_hashes - 1], d.nonce, e.msg);
        tx->fee = bumped; /* the next attempt bumps from here */
        return;
    }
    av_log(AV_LOG_WARN, "writer",
           "tx %s (nonce %" PRIu64 ") not mined after %u blocks: replaced by %s (max fee %" PRIu64
           " wei, tip %" PRIu64 " wei)",
           tx->hashes[tx->n_hashes - 1], d.nonce, w->cfg.replace_after_blocks, hash, d.max_fee_per_gas,
           d.max_priority_fee_per_gas);
    memcpy(tx->hashes[tx->n_hashes++], hash, 67);
    tx->desc = d;
    tx->fee = bumped;
    av_metric_inc(g_m_replacements, NULL, 1);
}

/* Keys of the EntityCreated logs (Arkiv contract only) of one receipt, in log order. */
static size_t receipt_created_keys(yyjson_val *receipt, const uint8_t contract[20], uint8_t (*keys)[32],
                                   size_t max)
{
    size_t n = 0;
    yyjson_val *logs = yyjson_obj_get(receipt, "logs");
    size_t idx, mx;
    yyjson_val *lg;
    if (!yyjson_is_arr(logs))
        return 0;
    yyjson_arr_foreach(logs, idx, mx, lg)
    {
        uint8_t addr[20];
        const char *as = yyjson_get_str(yyjson_obj_get(lg, "address"));
        if (as == NULL || !av_addr_from_hex(as, addr) || memcmp(addr, contract, 20) != 0)
            continue;
        av_event ev;
        bool known = false;
        av_err pe;
        if (av_event_parse(lg, &ev, &known, &pe) != AV_OK || !known || ev.kind != AV_EV_CREATED)
            continue;
        if (n < max)
            memcpy(keys[n], ev.key, 32);
        n++;
    }
    return n;
}

static void receipt_metrics(const pending_tx *tx, yyjson_val *rc)
{
    uint64_t gas_used = 0, price = 0;
    const char *gu = yyjson_get_str(yyjson_obj_get(rc, "gasUsed"));
    const char *ep = yyjson_get_str(yyjson_obj_get(rc, "effectiveGasPrice"));
    bool has_gu = gu != NULL && av_qty_parse(gu, &gas_used);
    bool has_ep = ep != NULL && av_qty_parse(ep, &price);
    if (has_gu) {
        av_metric_inc(g_m_gas_used, NULL, (double) gas_used);
        av_metric_inc(g_m_gas_reserved, NULL, (double) tx->desc.gas_limit);
    }
    if (has_ep)
        av_metric_set(g_m_gas_price, NULL, (double) price);
    av_log(AV_LOG_INFO, "writer",
           "tx %s mined in block %" PRIu64 ": gas used %" PRIu64 " of %" PRIu64
           " reserved, effective gas price %" PRIu64 " wei",
           tx->mined_hash, tx->block, gas_used, tx->desc.gas_limit, price);
}

/*
 * Poll receipts for every transaction (all of its hashes). A transaction not mined after
 * replace_after_blocks blocks is resent with bumped fees (max_replacements times). Created-entity
 * keys are taken from the receipt logs when kp is given.
 */
static av_status wait_receipts(av_writer *w, pending_tx *txs, size_t n, int timeout_ms, key_plan *kp,
                               av_arena *arena, uint64_t *highest, av_err *e)
{
    *highest = 0;
    if (w->rpc == NULL)
        return AV_OK;
    int max_wait = timeout_ms > 0 ? timeout_ms : 60000;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t replace_after = w->cfg.replace_after_blocks > 0 ? w->cfg.replace_after_blocks : 10;
    uint32_t max_repl = w->cfg.no_replace ? 0 : w->cfg.max_replacements;
    if (max_repl > AV_MAX_REPLACEMENTS)
        max_repl = AV_MAX_REPLACEMENTS;

    for (size_t i = 0; i < n; i++) {
        pending_tx *tx = &txs[i];
        int sleep_ms = 50;
        uint64_t last_head_check = 0;
        for (;;) {
            for (size_t h = 0; h < tx->n_hashes && tx->mined_hash[0] == '\0'; h++) {
                char qparams[96];
                snprintf(qparams, sizeof(qparams), "[\"%s\"]", tx->hashes[h]);
                yyjson_doc *rc_doc = NULL;
                yyjson_val *rc = NULL;
                int rc_code = 0;
                av_err pe;
                av_err_clear(&pe);
                if (av_rpc_call(w->rpc, "eth_getTransactionReceipt", qparams, &rc_doc, &rc, &rc_code, &pe) !=
                        AV_OK ||
                    rc == NULL || yyjson_is_null(rc)) {
                    if (rc_doc != NULL)
                        yyjson_doc_free(rc_doc);
                    continue;
                }
                const char *st_str = yyjson_get_str(yyjson_obj_get(rc, "status"));
                if (st_str != NULL && strcmp(st_str, "0x0") == 0) {
                    yyjson_doc_free(rc_doc);
                    invalidate_nonces(w);
                    av_err_set(e, "tx_reverted", "transaction %s reverted on-chain", tx->hashes[h]);
                    return AV_ERR;
                }
                const char *bn = yyjson_get_str(yyjson_obj_get(rc, "blockNumber"));
                if (bn != NULL)
                    (void) av_qty_parse(bn, &tx->block);
                memcpy(tx->mined_hash, tx->hashes[h], 67);
                receipt_metrics(tx, rc);
                if (kp != NULL && kp->results != NULL && tx->n_creates > 0) {
                    uint8_t (*keys)[32] = av_arena_alloc(arena, tx->n_creates * 32, 1);
                    size_t got = keys != NULL
                                     ? receipt_created_keys(rc, w->cfg.contract_addr, keys, tx->n_creates)
                                     : 0;
                    if (got == tx->n_creates) {
                        for (size_t c = 0; c < tx->n_creates; c++) {
                            uint8_t *dst = kp->results[kp->create_res[tx->first_create + c]].key;
                            if (memcmp(dst, keys[c], 32) != 0) {
                                char pk[67], rk[67];
                                av_key_to_hex(dst, pk);
                                av_key_to_hex(keys[c], rk);
                                av_log(AV_LOG_WARN, "writer",
                                       "predicted key %s differs from the mined key %s", pk, rk);
                                memcpy(dst, keys[c], 32);
                            }
                        }
                    } else {
                        av_log(AV_LOG_WARN, "writer", "receipt %s has %zu EntityCreated logs for %zu creates",
                               tx->mined_hash, got, tx->n_creates);
                    }
                }
                yyjson_doc_free(rc_doc);
            }
            if (tx->mined_hash[0] != '\0')
                break;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (long) (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
            if (elapsed >= max_wait) {
                invalidate_nonces(w);
                av_err_set(e, "consistency_timeout", "timeout waiting for the receipt of transaction %s",
                           tx->hashes[tx->n_hashes - 1]);
                return AV_ERR;
            }
            /* Stuck? Check the head about every 500 ms. */
            if (tx->n_hashes <= max_repl && (uint64_t) elapsed >= last_head_check + 500) {
                last_head_check = (uint64_t) elapsed;
                uint64_t head = 0;
                av_err be;
                av_err_clear(&be);
                if (av_rpc_block_number(w->rpc, &head, &be) == AV_OK &&
                    head >= tx->sent_block + replace_after) {
                    replace_tx(w, tx, arena);
                    tx->sent_block = head;
                }
            }
            usleep((useconds_t) (sleep_ms * 1000));
            if (sleep_ms < 250)
                sleep_ms += 50;
        }
        if (tx->block > *highest)
            *highest = tx->block;
    }
    return AV_OK;
}

/* Submit, then wait as requested. tx hashes reported are the mined ones (after any replacement). */
static av_status broadcast_and_wait(av_writer *w, av_collection *col, av_op_batch *batches, size_t n_batches,
                                    const av_mutation_op *ops_base, key_plan *kp,
                                    av_write_wait_mode wait_mode, int timeout_ms, av_arena *arena,
                                    char (**out_hashes)[67], size_t *out_n_hashes, uint64_t *out_block_number,
                                    bool *has_block, av_err *e)
{
    *out_n_hashes = 0;
    *has_block = false;
    *out_block_number = 0;
    if (n_batches == 0)
        return AV_OK;

    pending_tx *txs = NULL;
    if (submit_batches(w, batches, n_batches, ops_base, kp, arena, &txs, e) != AV_OK)
        return AV_ERR;

    char (*tx_hashes)[67] = av_arena_alloc(arena, n_batches * sizeof(*tx_hashes), 1);
    if (tx_hashes == NULL) {
        av_err_set(e, "internal", "out of memory allocating tx hashes");
        return AV_ERR;
    }
    uint64_t highest = 0;
    if (wait_mode != AV_WAIT_NONE &&
        wait_receipts(w, txs, n_batches, timeout_ms, kp, arena, &highest, e) != AV_OK)
        return AV_ERR;
    for (size_t i = 0; i < n_batches; i++)
        memcpy(tx_hashes[i], txs[i].mined_hash[0] != '\0' ? txs[i].mined_hash : txs[i].hashes[0], 67);
    *out_hashes = tx_hashes;
    *out_n_hashes = n_batches;

    if (wait_mode != AV_WAIT_NONE && highest > 0) {
        *out_block_number = highest;
        *has_block = true;
        if (wait_mode == AV_WAIT_INDEXED && col != NULL) {
            if (av_collection_wait_block(col, highest, timeout_ms, e) != AV_OK)
                return AV_ERR;
        }
    }
    return AV_OK;
}

av_status av_writer_upsert(av_writer *w, av_collection *col, yyjson_val *entities_arr,
                           av_write_wait_mode wait_mode, int timeout_ms, av_arena *arena,
                           av_upsert_batch_result *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (w == NULL || w->signer == NULL) {
        av_err_set(e, "write_disabled", "daemon is in read-only mode (no signer configured)");
        return AV_ERR;
    }
    if (col == NULL || entities_arr == NULL || !yyjson_is_arr(entities_arr)) {
        av_err_set(e, "invalid_argument", "entities must be an array");
        return AV_ERR;
    }

    size_t count = yyjson_arr_size(entities_arr);
    if (count == 0)
        return AV_OK;

    const av_coldef *coldef = av_collection_def(col);
    const uint8_t *signer_addr = av_signer_get_address(w->signer);

    /* Allocate operations and item results */
    av_mutation_op *ops = av_arena_alloc(arena, count * 2 * sizeof(av_mutation_op), 0);
    av_upsert_item_result *results = av_arena_alloc(arena, count * sizeof(av_upsert_item_result), 0);
    if (ops == NULL || results == NULL) {
        av_err_set(e, "internal", "out of memory allocating upsert operations");
        return AV_ERR;
    }

    size_t n_ops = 0;
    size_t n_creates = 0;
    size_t *create_res = av_arena_alloc(arena, count * sizeof(size_t), _Alignof(size_t));
    if (create_res == NULL) {
        av_err_set(e, "internal", "out of memory allocating upsert operations");
        return AV_ERR;
    }

    for (size_t i = 0; i < count; i++) {
        yyjson_val *ent_val = yyjson_arr_get(entities_arr, i);
        if (!yyjson_is_obj(ent_val)) {
            av_err_set(e, "invalid_argument", "entities[%zu] must be an object", i);
            return AV_ERR;
        }

        yyjson_val *id_val = yyjson_obj_get(ent_val, "id");
        const char *id_str = yyjson_get_str(id_val);
        if (id_str != NULL) {
            strncpy(results[i].id, id_str, sizeof(results[i].id) - 1);
        } else {
            results[i].id[0] = '\0';
        }

        /* Check if entity already exists */
        uint8_t existing_key[32];
        bool has_existing = false;
        av_row existing_row;
        if (id_str != NULL && w->store != NULL) {
            if (av_store_get_id(w->store, coldef->name, id_str, strlen(id_str), existing_key, &has_existing,
                                e) == AV_OK &&
                has_existing) {
                (void) av_store_get_entity(w->store, coldef->name, existing_key, arena, &existing_row,
                                           &has_existing, e);
            }
        }

        /* Check ownership */
        if (has_existing) {
            if (memcmp(existing_row.owner, signer_addr, 20) != 0) {
                av_err_set(e, "not_owner", "entity '%s' is owned by another account", id_str);
                return AV_ERR;
            }
        }

        /* Parse TTL */
        uint64_t exp_at = 0, min_life = 0;
        yyjson_val *ttl_val = yyjson_obj_get(ent_val, "ttl");
        bool has_ttl = ttl_val != NULL;
        if (ttl_val != NULL) {
            if (!parse_ttl_obj(ttl_val, &exp_at, &min_life, e))
                return AV_ERR;
        }

        /* Parse vector */
        yyjson_val *vec_val = yyjson_obj_get(ent_val, "vector");
        yyjson_val *vb_val = yyjson_obj_get(ent_val, "vector_bytes");
        uint8_t *vecbytes = NULL;
        size_t vecbytes_len = 0;
        if (parse_entity_vector(vec_val, vb_val, coldef, arena, &vecbytes, &vecbytes_len, e) != AV_OK) {
            return AV_ERR;
        }

        /* Parse user attributes */
        yyjson_val *attrs_obj = yyjson_obj_get(ent_val, "attributes");
        size_t n_user_attrs =
            (attrs_obj != NULL && yyjson_is_obj(attrs_obj)) ? yyjson_obj_size(attrs_obj) : 0;
        size_t total_attrs = n_user_attrs + 4; /* schema, id, vec, doc */

        av_attr *attr_list = av_arena_alloc(arena, total_attrs * sizeof(av_attr), 0);
        size_t a_idx = 0;

        /* System attr: arkiv.schema */
        strncpy(attr_list[a_idx].name, AV_ATTR_SCHEMA, sizeof(attr_list[a_idx].name) - 1);
        attr_list[a_idx].type = AV_AT_STR;
        attr_list[a_idx].indexed = true;
        attr_list[a_idx].val = (const uint8_t *) coldef->schema_tag;
        attr_list[a_idx].len = strlen(coldef->schema_tag);
        a_idx++;

        /* System attr: av.id */
        if (id_str != NULL) {
            strncpy(attr_list[a_idx].name, AV_ATTR_ID, sizeof(attr_list[a_idx].name) - 1);
            attr_list[a_idx].type = AV_AT_STR;
            attr_list[a_idx].indexed = true;
            attr_list[a_idx].val = (const uint8_t *) id_str;
            attr_list[a_idx].len = strlen(id_str);
            a_idx++;
        }

        /* System attr: av.vec */
        if (vecbytes != NULL) {
            strncpy(attr_list[a_idx].name, AV_ATTR_VEC, sizeof(attr_list[a_idx].name) - 1);
            attr_list[a_idx].type = AV_AT_BYTES;
            attr_list[a_idx].indexed = false;
            attr_list[a_idx].val = vecbytes;
            attr_list[a_idx].len = vecbytes_len;
            a_idx++;
        }

        /* System attr: av.doc */
        yyjson_val *doc_val = yyjson_obj_get(ent_val, "document");
        const char *doc_str = yyjson_get_str(doc_val);
        if (doc_str != NULL) {
            strncpy(attr_list[a_idx].name, AV_ATTR_DOC, sizeof(attr_list[a_idx].name) - 1);
            attr_list[a_idx].type = AV_AT_BYTES;
            attr_list[a_idx].indexed = false;
            attr_list[a_idx].val = (const uint8_t *) doc_str;
            attr_list[a_idx].len = strlen(doc_str);
            a_idx++;
        }

        /* User attributes (TypedValue or shorthand typed from the collection schema) */
        if (attrs_obj != NULL && yyjson_is_obj(attrs_obj)) {
            yyjson_obj_iter iter;
            yyjson_obj_iter_init(attrs_obj, &iter);
            yyjson_val *key_node = NULL;
            while ((key_node = yyjson_obj_iter_next(&iter)) != NULL) {
                if (a_idx >= total_attrs)
                    break;
                if (build_user_attr(coldef, yyjson_get_str(key_node), yyjson_obj_iter_get_val(key_node),
                                    arena, &attr_list[a_idx], e) != AV_OK)
                    return AV_ERR;
                a_idx++;
            }
        }

        if (has_existing) {
            /* PATCH */
            results[i].op = "patch";
            memcpy(results[i].key, existing_key, 32);

            ops[n_ops].type = AV_OP_PATCH;
            memcpy(ops[n_ops].u.patch.entity_key, existing_key, 32);
            ops[n_ops].u.patch.mutations = attr_list;
            ops[n_ops].u.patch.n_mutations = a_idx;
            n_ops++;

            if (ttl_val != NULL) {
                ops[n_ops].type = AV_OP_EXTEND_EXPIRY;
                memcpy(ops[n_ops].u.extend.entity_key, existing_key, 32);
                ops[n_ops].u.extend.expires_at = exp_at;
                ops[n_ops].u.extend.min_lifetime = min_life;
                n_ops++;
            }
        } else {
            /* CREATE */
            if (!has_ttl && !apply_default_ttl(coldef, &exp_at, &min_life, e))
                return AV_ERR;
            results[i].op = "create";
            uint8_t salt[16];
            for (int s = 0; s < 16; s++) {
                salt[s] = (uint8_t) (rand() & 0xff);
            }

            memset(results[i].key, 0, 32); /* assigned in submit_batches / from the receipt */
            create_res[n_creates++] = i;

            ops[n_ops].type = AV_OP_CREATE;
            memcpy(ops[n_ops].u.create.salt, salt, 16);
            ops[n_ops].u.create.expires_at = exp_at;
            ops[n_ops].u.create.min_lifetime = min_life;
            ops[n_ops].u.create.creation_flags = 0;
            ops[n_ops].u.create.attributes = attr_list;
            ops[n_ops].u.create.n_attributes = a_idx;
            n_ops++;
        }
    }

    out->results = results;
    out->n_results = count;

    if (n_ops == 0)
        return AV_OK;

    av_op_batch *batches = NULL;
    size_t n_batches = 0;
    if (av_batcher_pack(ops, n_ops, &w->cfg.batcher, arena, &batches, &n_batches, e) != AV_OK)
        return AV_ERR;

    char (*tx_hashes)[67] = NULL;
    size_t n_tx_hashes = 0;
    uint64_t block_number = 0;
    bool has_block = false;

    key_plan kp = {results, create_res, n_creates, wait_mode == AV_WAIT_NONE};
    if (broadcast_and_wait(w, col, batches, n_batches, ops, &kp, wait_mode, timeout_ms, arena, &tx_hashes,
                           &n_tx_hashes, &block_number, &has_block, e) != AV_OK) {
        return AV_ERR;
    }

    out->tx_hashes = tx_hashes;
    out->n_tx_hashes = n_tx_hashes;
    out->block_number = block_number;
    out->has_block_number = has_block;

    return AV_OK;
}

av_status av_writer_delete_refs(av_writer *w, av_collection *col, const char *const *refs, size_t n_refs,
                                av_arena *arena, av_tx_response *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (w == NULL || w->signer == NULL) {
        av_err_set(e, "write_disabled", "daemon is in read-only mode");
        return AV_ERR;
    }
    if (n_refs == 0)
        return AV_OK;

    const uint8_t *signer_addr = av_signer_get_address(w->signer);
    av_mutation_op *ops = av_arena_alloc(arena, n_refs * sizeof(av_mutation_op), 0);
    if (ops == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    const av_coldef *def = av_collection_def(col);
    size_t n_ops = 0;

    for (size_t i = 0; i < n_refs; i++) {
        uint8_t key[32];
        if (!av_key_from_hex(refs[i], key)) {
            bool found = false;
            if (av_store_get_id(w->store, def->name, refs[i], strlen(refs[i]), key, &found, e) != AV_OK ||
                !found) {
                av_err_set(e, "entity_not_found", "entity not found: %s", refs[i]);
                return AV_ERR;
            }
        }

        av_row row;
        bool found = false;
        if (av_store_get_entity(w->store, def->name, key, arena, &row, &found, e) != AV_OK || !found) {
            av_err_set(e, "entity_not_found", "entity not found: %s", refs[i]);
            return AV_ERR;
        }

        if (memcmp(row.owner, signer_addr, 20) != 0) {
            av_err_set(e, "not_owner", "entity is not owned by the signer");
            return AV_ERR;
        }

        ops[n_ops].type = AV_OP_DELETE;
        memcpy(ops[n_ops].u.del.entity_key, key, 32);
        n_ops++;
    }

    av_op_batch *batches = NULL;
    size_t n_batches = 0;
    if (av_batcher_pack(ops, n_ops, &w->cfg.batcher, arena, &batches, &n_batches, e) != AV_OK)
        return AV_ERR;

    char (*tx_hashes)[67] = NULL;
    size_t n_hashes = 0;
    uint64_t bnum = 0;
    bool has_b = false;
    if (broadcast_and_wait(w, col, batches, n_batches, ops, NULL, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes,
                           &n_hashes, &bnum, &has_b, e) != AV_OK)
        return AV_ERR;

    out->affected = n_ops;
    out->tx_hashes = tx_hashes;
    out->n_tx_hashes = n_hashes;
    out->block_number = bnum;
    return AV_OK;
}

av_status av_writer_delete_filter(av_writer *w, av_collection *col, const roaring_bitmap_t *matching,
                                  av_arena *arena, av_tx_response *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (w == NULL || w->signer == NULL) {
        av_err_set(e, "write_disabled", "daemon is in read-only mode");
        return AV_ERR;
    }
    if (matching == NULL)
        return AV_OK;

    uint64_t card = roaring_bitmap_get_cardinality(matching);
    if (card > 10000) {
        av_err_set(e, "invalid_argument", "filter matches %" PRIu64 " entities, exceeding 10000 cap", card);
        return AV_ERR;
    }
    if (card == 0)
        return AV_OK;

    const uint8_t *signer_addr = av_signer_get_address(w->signer);
    const av_coldef *def = av_collection_def(col);
    const av_docmap *docmap = av_collection_docmap(col);

    av_mutation_op *ops = av_arena_alloc(arena, (size_t) card * sizeof(av_mutation_op), 0);
    if (ops == NULL && card > 0) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    size_t n_ops = 0;
    roaring_uint32_iterator_t it;
    roaring_iterator_init(matching, &it);
    while (it.has_value) {
        av_docid docid = (av_docid) it.current_value;
        const uint8_t *key = av_docmap_key(docmap, docid);
        if (key != NULL) {
            av_row row;
            bool found = false;
            if (av_store_get_entity(w->store, def->name, key, arena, &row, &found, e) == AV_OK && found) {
                if (memcmp(row.owner, signer_addr, 20) == 0) {
                    ops[n_ops].type = AV_OP_DELETE;
                    memcpy(ops[n_ops].u.del.entity_key, key, 32);
                    n_ops++;
                }
            }
        }
        roaring_uint32_iterator_advance(&it);
    }

    av_op_batch *batches = NULL;
    size_t n_batches = 0;
    if (av_batcher_pack(ops, n_ops, &w->cfg.batcher, arena, &batches, &n_batches, e) != AV_OK)
        return AV_ERR;

    char (*tx_hashes)[67] = NULL;
    size_t n_hashes = 0;
    uint64_t bnum = 0;
    bool has_b = false;
    if (broadcast_and_wait(w, col, batches, n_batches, ops, NULL, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes,
                           &n_hashes, &bnum, &has_b, e) != AV_OK)
        return AV_ERR;

    out->affected = n_ops;
    out->tx_hashes = tx_hashes;
    out->n_tx_hashes = n_hashes;
    out->block_number = bnum;
    return AV_OK;
}

av_status av_writer_extend_refs(av_writer *w, av_collection *col, const char *const *refs, size_t n_refs,
                                uint64_t expires_at, uint64_t min_lifetime, av_arena *arena,
                                av_tx_response *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (w == NULL || w->signer == NULL) {
        av_err_set(e, "write_disabled", "daemon is in read-only mode");
        return AV_ERR;
    }
    if (n_refs == 0)
        return AV_OK;

    const uint8_t *signer_addr = av_signer_get_address(w->signer);
    const av_coldef *def = av_collection_def(col);

    av_mutation_op *ops = av_arena_alloc(arena, n_refs * sizeof(av_mutation_op), 0);
    if (ops == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    size_t n_ops = 0;
    for (size_t i = 0; i < n_refs; i++) {
        uint8_t key[32];
        if (!av_key_from_hex(refs[i], key)) {
            bool found = false;
            if (av_store_get_id(w->store, def->name, refs[i], strlen(refs[i]), key, &found, e) != AV_OK ||
                !found) {
                av_err_set(e, "entity_not_found", "entity not found: %s", refs[i]);
                return AV_ERR;
            }
        }

        av_row row;
        bool found = false;
        if (av_store_get_entity(w->store, def->name, key, arena, &row, &found, e) != AV_OK || !found) {
            av_err_set(e, "entity_not_found", "entity not found: %s", refs[i]);
            return AV_ERR;
        }

        if (memcmp(row.owner, signer_addr, 20) != 0) {
            av_err_set(e, "not_owner", "entity is not owned by the signer");
            return AV_ERR;
        }

        ops[n_ops].type = AV_OP_EXTEND_EXPIRY;
        memcpy(ops[n_ops].u.extend.entity_key, key, 32);
        ops[n_ops].u.extend.expires_at = expires_at;
        ops[n_ops].u.extend.min_lifetime = min_lifetime;
        n_ops++;
    }

    av_op_batch *batches = NULL;
    size_t n_batches = 0;
    if (av_batcher_pack(ops, n_ops, &w->cfg.batcher, arena, &batches, &n_batches, e) != AV_OK)
        return AV_ERR;

    char (*tx_hashes)[67] = NULL;
    size_t n_hashes = 0;
    uint64_t bnum = 0;
    bool has_b = false;
    if (broadcast_and_wait(w, col, batches, n_batches, ops, NULL, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes,
                           &n_hashes, &bnum, &has_b, e) != AV_OK)
        return AV_ERR;

    out->affected = n_ops;
    out->tx_hashes = tx_hashes;
    out->n_tx_hashes = n_hashes;
    out->block_number = bnum;
    return AV_OK;
}

av_status av_writer_extend_filter(av_writer *w, av_collection *col, const roaring_bitmap_t *matching,
                                  uint64_t expires_at, uint64_t min_lifetime, av_arena *arena,
                                  av_tx_response *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (w == NULL || w->signer == NULL) {
        av_err_set(e, "write_disabled", "daemon is in read-only mode");
        return AV_ERR;
    }
    if (matching == NULL)
        return AV_OK;

    uint64_t card = roaring_bitmap_get_cardinality(matching);
    if (card > 10000) {
        av_err_set(e, "invalid_argument", "filter matches %" PRIu64 " entities, exceeding 10000 cap", card);
        return AV_ERR;
    }
    if (card == 0)
        return AV_OK;

    const uint8_t *signer_addr = av_signer_get_address(w->signer);
    const av_coldef *def = av_collection_def(col);
    const av_docmap *docmap = av_collection_docmap(col);

    av_mutation_op *ops = av_arena_alloc(arena, (size_t) card * sizeof(av_mutation_op), 0);
    if (ops == NULL && card > 0) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    size_t n_ops = 0;
    roaring_uint32_iterator_t it;
    roaring_iterator_init(matching, &it);
    while (it.has_value) {
        av_docid docid = (av_docid) it.current_value;
        const uint8_t *key = av_docmap_key(docmap, docid);
        if (key != NULL) {
            av_row row;
            bool found = false;
            if (av_store_get_entity(w->store, def->name, key, arena, &row, &found, e) == AV_OK && found) {
                if (memcmp(row.owner, signer_addr, 20) == 0) {
                    ops[n_ops].type = AV_OP_EXTEND_EXPIRY;
                    memcpy(ops[n_ops].u.extend.entity_key, key, 32);
                    ops[n_ops].u.extend.expires_at = expires_at;
                    ops[n_ops].u.extend.min_lifetime = min_lifetime;
                    n_ops++;
                }
            }
        }
        roaring_uint32_iterator_advance(&it);
    }

    av_op_batch *batches = NULL;
    size_t n_batches = 0;
    if (av_batcher_pack(ops, n_ops, &w->cfg.batcher, arena, &batches, &n_batches, e) != AV_OK)
        return AV_ERR;

    char (*tx_hashes)[67] = NULL;
    size_t n_hashes = 0;
    uint64_t bnum = 0;
    bool has_b = false;
    if (broadcast_and_wait(w, col, batches, n_batches, ops, NULL, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes,
                           &n_hashes, &bnum, &has_b, e) != AV_OK)
        return AV_ERR;

    out->affected = n_ops;
    out->tx_hashes = tx_hashes;
    out->n_tx_hashes = n_hashes;
    out->block_number = bnum;
    return AV_OK;
}

av_status av_writer_prepare(av_writer *w, av_collection *col, yyjson_val *entities_arr,
                            const uint8_t from_addr[20], av_arena *arena, av_prepare_result *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (col == NULL || entities_arr == NULL || !yyjson_is_arr(entities_arr)) {
        av_err_set(e, "invalid_argument", "entities must be an array");
        return AV_ERR;
    }

    size_t count = yyjson_arr_size(entities_arr);
    if (count == 0)
        return AV_OK;

    const av_coldef *coldef = av_collection_def(col);
    av_mutation_op *ops = av_arena_alloc(arena, count * 2 * sizeof(av_mutation_op), 0);
    av_upsert_item_result *results = av_arena_alloc(arena, count * sizeof(av_upsert_item_result), 0);
    if (ops == NULL || results == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    size_t n_ops = 0;
    uint64_t create_counter = 0;
    /* Keys and nonces follow the chain state of `from` (issue #13): entityNonce and the pending
     * transaction count. Without an RPC (unit tests) both start at 0. */
    uint64_t first_tx_nonce = 0;
    if (w->rpc != NULL) {
        if (fetch_entity_nonce(w, from_addr, &create_counter, e) != AV_OK)
            return AV_ERR;
        char addr_hex[43];
        av_addr_to_hex(from_addr, addr_hex);
        char params[96];
        snprintf(params, sizeof(params), "[\"%s\",\"pending\"]", addr_hex);
        yyjson_doc *nd = NULL;
        yyjson_val *nv = NULL;
        int ncode = 0;
        if (av_rpc_call(w->rpc, "eth_getTransactionCount", params, &nd, &nv, &ncode, e) != AV_OK)
            return AV_ERR;
        const char *nh = yyjson_get_str(nv);
        bool ok = nh != NULL && av_qty_parse(nh, &first_tx_nonce);
        yyjson_doc_free(nd);
        if (!ok) {
            av_err_set(e, "arkiv_rpc_error", "eth_getTransactionCount returned a malformed value");
            return AV_ERR;
        }
    }

    for (size_t i = 0; i < count; i++) {
        yyjson_val *ent_val = yyjson_arr_get(entities_arr, i);
        if (!yyjson_is_obj(ent_val)) {
            av_err_set(e, "invalid_argument", "entities[%zu] must be an object", i);
            return AV_ERR;
        }

        yyjson_val *id_val = yyjson_obj_get(ent_val, "id");
        const char *id_str = yyjson_get_str(id_val);
        if (id_str != NULL) {
            strncpy(results[i].id, id_str, sizeof(results[i].id) - 1);
        } else {
            results[i].id[0] = '\0';
        }

        uint8_t existing_key[32];
        bool has_existing = false;
        av_row existing_row;
        if (id_str != NULL && w->store != NULL) {
            if (av_store_get_id(w->store, coldef->name, id_str, strlen(id_str), existing_key, &has_existing,
                                e) == AV_OK &&
                has_existing) {
                (void) av_store_get_entity(w->store, coldef->name, existing_key, arena, &existing_row,
                                           &has_existing, e);
            }
        }

        if (has_existing && memcmp(existing_row.owner, from_addr, 20) != 0) {
            av_err_set(e, "not_owner", "entity '%s' is owned by another account", id_str);
            return AV_ERR;
        }

        uint64_t exp_at = 0, min_life = 0;
        yyjson_val *ttl_val = yyjson_obj_get(ent_val, "ttl");
        bool has_ttl = ttl_val != NULL;
        if (ttl_val != NULL) {
            if (!parse_ttl_obj(ttl_val, &exp_at, &min_life, e))
                return AV_ERR;
        }

        yyjson_val *vec_val = yyjson_obj_get(ent_val, "vector");
        yyjson_val *vb_val = yyjson_obj_get(ent_val, "vector_bytes");
        uint8_t *vecbytes = NULL;
        size_t vecbytes_len = 0;
        if (parse_entity_vector(vec_val, vb_val, coldef, arena, &vecbytes, &vecbytes_len, e) != AV_OK) {
            return AV_ERR;
        }

        /* Parse user attributes */
        yyjson_val *attrs_obj = yyjson_obj_get(ent_val, "attributes");
        size_t n_user_attrs =
            (attrs_obj != NULL && yyjson_is_obj(attrs_obj)) ? yyjson_obj_size(attrs_obj) : 0;
        size_t total_attrs = n_user_attrs + 4; /* schema, id, vec, doc */

        av_attr *attr_list = av_arena_alloc(arena, total_attrs * sizeof(av_attr), 0);
        size_t a_idx = 0;

        /* System attr: arkiv.schema */
        strncpy(attr_list[a_idx].name, AV_ATTR_SCHEMA, sizeof(attr_list[a_idx].name) - 1);
        attr_list[a_idx].type = AV_AT_STR;
        attr_list[a_idx].indexed = true;
        attr_list[a_idx].val = (const uint8_t *) coldef->schema_tag;
        attr_list[a_idx].len = strlen(coldef->schema_tag);
        a_idx++;

        /* System attr: av.id */
        if (id_str != NULL) {
            strncpy(attr_list[a_idx].name, AV_ATTR_ID, sizeof(attr_list[a_idx].name) - 1);
            attr_list[a_idx].type = AV_AT_STR;
            attr_list[a_idx].indexed = true;
            attr_list[a_idx].val = (const uint8_t *) id_str;
            attr_list[a_idx].len = strlen(id_str);
            a_idx++;
        }

        /* System attr: av.vec */
        if (vecbytes != NULL) {
            strncpy(attr_list[a_idx].name, AV_ATTR_VEC, sizeof(attr_list[a_idx].name) - 1);
            attr_list[a_idx].type = AV_AT_BYTES;
            attr_list[a_idx].indexed = false;
            attr_list[a_idx].val = vecbytes;
            attr_list[a_idx].len = vecbytes_len;
            a_idx++;
        }

        /* System attr: av.doc */
        yyjson_val *doc_val = yyjson_obj_get(ent_val, "document");
        const char *doc_str = yyjson_get_str(doc_val);
        if (doc_str != NULL) {
            strncpy(attr_list[a_idx].name, AV_ATTR_DOC, sizeof(attr_list[a_idx].name) - 1);
            attr_list[a_idx].type = AV_AT_BYTES;
            attr_list[a_idx].indexed = false;
            attr_list[a_idx].val = (const uint8_t *) doc_str;
            attr_list[a_idx].len = strlen(doc_str);
            a_idx++;
        }

        /* User attributes (TypedValue or shorthand typed from the collection schema) */
        if (attrs_obj != NULL && yyjson_is_obj(attrs_obj)) {
            yyjson_obj_iter iter;
            yyjson_obj_iter_init(attrs_obj, &iter);
            yyjson_val *key_node = NULL;
            while ((key_node = yyjson_obj_iter_next(&iter)) != NULL) {
                if (a_idx >= total_attrs)
                    break;
                if (build_user_attr(coldef, yyjson_get_str(key_node), yyjson_obj_iter_get_val(key_node),
                                    arena, &attr_list[a_idx], e) != AV_OK)
                    return AV_ERR;
                a_idx++;
            }
        }

        if (has_existing) {
            results[i].op = "patch";
            memcpy(results[i].key, existing_key, 32);

            ops[n_ops].type = AV_OP_PATCH;
            memcpy(ops[n_ops].u.patch.entity_key, existing_key, 32);
            ops[n_ops].u.patch.mutations = attr_list;
            ops[n_ops].u.patch.n_mutations = a_idx;
            n_ops++;

            if (ttl_val != NULL) {
                ops[n_ops].type = AV_OP_EXTEND_EXPIRY;
                memcpy(ops[n_ops].u.extend.entity_key, existing_key, 32);
                ops[n_ops].u.extend.expires_at = exp_at;
                ops[n_ops].u.extend.min_lifetime = min_life;
                n_ops++;
            }
        } else {
            if (!has_ttl && !apply_default_ttl(coldef, &exp_at, &min_life, e))
                return AV_ERR;
            results[i].op = "create";
            uint8_t salt[16];
            for (int s = 0; s < 16; s++) {
                salt[s] = (uint8_t) (rand() & 0xff);
            }

            av_abi_predict_entity_key(w->cfg.chain_id, w->cfg.contract_addr, from_addr, create_counter, salt,
                                      results[i].key);
            create_counter++;

            ops[n_ops].type = AV_OP_CREATE;
            memcpy(ops[n_ops].u.create.salt, salt, 16);
            ops[n_ops].u.create.expires_at = exp_at;
            ops[n_ops].u.create.min_lifetime = min_life;
            ops[n_ops].u.create.creation_flags = 0;
            ops[n_ops].u.create.attributes = attr_list;
            ops[n_ops].u.create.n_attributes = a_idx;
            n_ops++;
        }
    }

    out->results = results;
    out->n_results = count;
    out->chain_id = w->cfg.chain_id;

    if (n_ops == 0)
        return AV_OK;

    {
        av_fee_config fc = fee_config(w);
        av_fee_quote fee;
        av_fee_quote_rpc(w->rpc, &fc, &fee);
        out->max_fee_per_gas = fee.max_fee_per_gas;
        out->max_priority_fee_per_gas = fee.max_priority_fee_per_gas;
        out->fee_source = av_fee_source_name(fee.source);
    }

    av_op_batch *batches = NULL;
    size_t n_batches = 0;
    if (av_batcher_pack(ops, n_ops, &w->cfg.batcher, arena, &batches, &n_batches, e) != AV_OK)
        return AV_ERR;

    av_prepared_tx *txs = av_arena_alloc(arena, n_batches * sizeof(av_prepared_tx), 0);
    if (txs == NULL) {
        av_err_set(e, "internal", "out of memory allocating prepared txs");
        return AV_ERR;
    }

    for (size_t b = 0; b < n_batches; b++) {
        snprintf(txs[b].to, sizeof(txs[b].to), "%s", AV_ARKIV_CONTRACT_ADDR);
        snprintf(txs[b].value, sizeof(txs[b].value), "%s", "0x0");
        txs[b].nonce = first_tx_nonce + b;
        bool fb = false;
        txs[b].gas_estimate = estimate_gas_limit(w, from_addr, batches[b].calldata, batches[b].calldata_len,
                                                 batches[b].n_ops, arena, &fb);

        size_t c_len = batches[b].calldata_len;
        char *hex_str = av_arena_alloc(arena, c_len * 2 + 3, 0);
        hex_str[0] = '0';
        hex_str[1] = 'x';
        static const char hex_chars[] = "0123456789abcdef";
        for (size_t j = 0; j < c_len; j++) {
            hex_str[2 + j * 2] = hex_chars[(batches[b].calldata[j] >> 4) & 0x0f];
            hex_str[2 + j * 2 + 1] = hex_chars[batches[b].calldata[j] & 0x0f];
        }
        hex_str[2 + c_len * 2] = '\0';
        txs[b].data = hex_str;
    }

    out->transactions = txs;
    out->n_transactions = n_batches;
    return AV_OK;
}

/* ---- submit (transactions signed outside the daemon) ---- */

static bool coldef_allows_sender(const av_coldef *def, const uint8_t sender[20])
{
    if (def->membership == AV_MEMBERSHIP_OPEN || def->n_writers == 0)
        return true;
    for (size_t i = 0; i < def->n_writers; i++)
        if (memcmp(def->writers[i], sender, 20) == 0)
            return true;
    return false;
}

static bool is_tx_hash(const char *s)
{
    if (s == NULL || strlen(s) != 66 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return false;
    for (size_t i = 2; i < 66; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

/* eth_sendRawTransaction; "already known" counts as accepted so a retried submit is harmless. */
static av_status broadcast_raw(av_writer *w, const uint8_t *raw, size_t raw_len, size_t idx, av_arena *arena,
                               av_err *e)
{
    if (w->rpc == NULL) {
        av_err_set(e, "arkiv_rpc_error", "no Arkiv RPC configured to broadcast transactions");
        return AV_ERR;
    }
    char *raw_hex = hex_alloc(arena, raw, raw_len);
    size_t cap = raw_len * 2 + 16;
    char *params = av_arena_alloc(arena, cap, 1);
    if (raw_hex == NULL || params == NULL) {
        av_err_set(e, "internal", "out of memory for raw transaction");
        return AV_ERR;
    }
    snprintf(params, cap, "[\"%s\"]", raw_hex);
    yyjson_doc *doc = NULL;
    yyjson_val *res = NULL;
    int code = 0;
    av_status st = av_rpc_call(w->rpc, "eth_sendRawTransaction", params, &doc, &res, &code, e);
    if (doc != NULL)
        yyjson_doc_free(doc);
    if (st == AV_OK) {
        av_metric_inc(g_m_txs, NULL, 1);
        return AV_OK;
    }
    if (code != 0) {
        char msg[sizeof(e->msg)];
        snprintf(msg, sizeof(msg), "%s", e->msg);
        if (strstr(msg, "already known") != NULL || strstr(msg, "known transaction") != NULL) {
            av_err_clear(e);
            return AV_OK;
        }
        av_err_set(e, "tx_reverted", "signed_transactions[%zu] rejected by the node: %s", idx, msg);
        av_err_field(e, "/signed_transactions");
    }
    return AV_ERR;
}

/* Poll one receipt until mined or the deadline. Fills block, appends EntityCreated keys. */
static av_status poll_receipt(av_writer *w, const av_coldef *def, const char *hash, const struct timespec *t0,
                              int max_wait, av_arena *arena, uint64_t *block, uint8_t (**keys)[32],
                              size_t *n_keys, size_t *cap_keys, av_err *e)
{
    int sleep_ms = 50;
    for (;;) {
        char qparams[96];
        snprintf(qparams, sizeof(qparams), "[\"%s\"]", hash);
        yyjson_doc *doc = NULL;
        yyjson_val *rc = NULL;
        int code = 0;
        av_err pe;
        av_err_clear(&pe);
        if (av_rpc_call(w->rpc, "eth_getTransactionReceipt", qparams, &doc, &rc, &code, &pe) == AV_OK &&
            rc != NULL && !yyjson_is_null(rc)) {
            const char *st_str = yyjson_get_str(yyjson_obj_get(rc, "status"));
            const char *to_s = yyjson_get_str(yyjson_obj_get(rc, "to"));
            const char *from_s = yyjson_get_str(yyjson_obj_get(rc, "from"));
            uint8_t to[20], from[20];
            av_status res = AV_OK;
            if (to_s == NULL || !av_addr_from_hex(to_s, to) || memcmp(to, w->cfg.contract_addr, 20) != 0) {
                av_err_set(e, "invalid_argument", "transaction %s is not a call to the Arkiv contract", hash);
                av_err_field(e, "/tx_hashes");
                res = AV_ERR;
            } else if (from_s != NULL && av_addr_from_hex(from_s, from) && !coldef_allows_sender(def, from)) {
                av_err_set(
                    e, "forbidden",
                    "transaction %s was sent by %s, which is not a writer of collection '%s'; it will not "
                    "be indexed",
                    hash, from_s, def->name);
                res = AV_ERR;
            } else if (st_str != NULL && strcmp(st_str, "0x0") == 0) {
                av_err_set(e, "tx_reverted", "transaction %s reverted on-chain", hash);
                res = AV_ERR;
            } else {
                const char *bn = yyjson_get_str(yyjson_obj_get(rc, "blockNumber"));
                if (bn != NULL)
                    (void) av_qty_parse(bn, block);
                size_t n = receipt_created_keys(rc, w->cfg.contract_addr, NULL, 0);
                if (n > 0) {
                    if (*n_keys + n > *cap_keys) {
                        size_t nc = (*cap_keys == 0 ? 16 : *cap_keys * 2);
                        while (nc < *n_keys + n)
                            nc *= 2;
                        uint8_t (*nk)[32] = av_arena_alloc(arena, nc * 32, 1);
                        if (nk == NULL) {
                            yyjson_doc_free(doc);
                            av_err_set(e, "internal", "out of memory collecting entity keys");
                            return AV_ERR;
                        }
                        if (*n_keys > 0)
                            memcpy(nk, *keys, *n_keys * 32);
                        *keys = nk;
                        *cap_keys = nc;
                    }
                    *n_keys += receipt_created_keys(rc, w->cfg.contract_addr, *keys + *n_keys, n);
                }
            }
            yyjson_doc_free(doc);
            return res;
        }
        if (doc != NULL)
            yyjson_doc_free(doc);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (long) (now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
        if (elapsed >= max_wait) {
            av_err_set(e, "consistency_timeout", "timeout waiting for the receipt of transaction %s", hash);
            return AV_ERR;
        }
        usleep((useconds_t) (sleep_ms * 1000));
        if (sleep_ms < 250)
            sleep_ms += 50;
    }
}

av_status av_writer_submit(av_writer *w, av_collection *col, const char *const *raw_hex,
                           const size_t *raw_hex_len, size_t n_raw, const char *const *hashes,
                           size_t n_hashes, av_write_wait_mode wait_mode, int timeout_ms, av_arena *arena,
                           av_submit_result *out, av_err *e)
{
    (void) pthread_once(&g_metrics_once, writer_metrics_init);
    memset(out, 0, sizeof(*out));
    if (w == NULL || col == NULL || (n_raw == 0) == (n_hashes == 0)) {
        av_err_set(e, "invalid_argument", "give exactly one of signed_transactions or tx_hashes");
        return AV_ERR;
    }
    const av_coldef *def = av_collection_def(col);
    size_t n = n_raw > 0 ? n_raw : n_hashes;
    char (*tx_hashes)[67] = av_arena_alloc(arena, n * sizeof(*tx_hashes), 1);
    if (tx_hashes == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    if (n_raw > 0) {
        /* Decode and check every transaction before broadcasting any of them. */
        uint8_t **raws = av_arena_alloc(arena, n_raw * sizeof(*raws), _Alignof(uint8_t *));
        size_t *lens = av_arena_alloc(arena, n_raw * sizeof(*lens), _Alignof(size_t));
        if (raws == NULL || lens == NULL) {
            av_err_set(e, "internal", "out of memory");
            return AV_ERR;
        }
        const uint8_t *own = av_writer_get_address(w);
        bool touches_own = false;
        for (size_t i = 0; i < n_raw; i++) {
            av_rawtx rt;
            if (av_rawtx_decode_hex(raw_hex[i], raw_hex_len[i], arena, &raws[i], &lens[i], &rt, e) != AV_OK) {
                char msg[sizeof(e->msg)];
                snprintf(msg, sizeof(msg), "%s", e->msg);
                av_err_set(e, "invalid_argument", "signed_transactions[%zu]: %s", i, msg);
                av_err_field(e, "/signed_transactions");
                return AV_ERR;
            }
            const char *why = NULL;
            if (rt.chain_id != w->cfg.chain_id)
                why = "chain id does not match the daemon's Arkiv chain";
            else if (!rt.has_to || memcmp(rt.to, w->cfg.contract_addr, 20) != 0)
                why = "'to' is not the Arkiv contract";
            else if (!rt.value_zero)
                why = "'value' must be 0";
            else if (rt.data_len < 4 || rt.data[0] != 0x49 || rt.data[1] != 0x65 || rt.data[2] != 0x00 ||
                     rt.data[3] != 0x44)
                why = "calldata is not an Arkiv execute(...) call";
            if (why != NULL) {
                av_err_set(e, "invalid_argument", "signed_transactions[%zu]: %s", i, why);
                av_err_field(e, "/signed_transactions");
                return AV_ERR;
            }
            if (!coldef_allows_sender(def, rt.sender)) {
                char sh[43];
                av_addr_to_hex(rt.sender, sh);
                av_err_set(
                    e, "forbidden",
                    "signed_transactions[%zu] is signed by %s, which is not a writer of collection '%s'", i,
                    sh, def->name);
                return AV_ERR;
            }
            if (own != NULL && memcmp(own, rt.sender, 20) == 0)
                touches_own = true;
            av_key_to_hex(rt.hash, tx_hashes[i]);
        }
        /* Transactions from the daemon's own signer address move its nonces: re-read them next time. */
        if (touches_own)
            invalidate_nonces(w);
        for (size_t i = 0; i < n_raw; i++) {
            if (broadcast_raw(w, raws[i], lens[i], i, arena, e) != AV_OK)
                return AV_ERR;
            av_log(AV_LOG_INFO, "writer", "relayed client-signed tx %s (%zu bytes)", tx_hashes[i], lens[i]);
        }
    } else {
        for (size_t i = 0; i < n_hashes; i++) {
            if (!is_tx_hash(hashes[i])) {
                av_err_set(e, "invalid_argument", "tx_hashes[%zu] is not a 32-byte hex transaction hash", i);
                av_err_field(e, "/tx_hashes");
                return AV_ERR;
            }
            for (size_t c = 0; c < 66; c++)
                tx_hashes[i][c] = (char) tolower((unsigned char) hashes[i][c]);
            tx_hashes[i][66] = '\0';
        }
    }
    out->tx_hashes = tx_hashes;
    out->n_tx_hashes = n;
    if (wait_mode == AV_WAIT_NONE)
        return AV_OK;
    if (w->rpc == NULL) {
        av_err_set(e, "arkiv_rpc_error", "no Arkiv RPC configured to read receipts");
        return AV_ERR;
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int max_wait = timeout_ms > 0 ? timeout_ms : 60000;
    uint64_t highest = 0;
    size_t cap_keys = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t block = 0;
        if (poll_receipt(w, def, tx_hashes[i], &t0, max_wait, arena, &block, &out->created_keys,
                         &out->n_created_keys, &cap_keys, e) != AV_OK)
            return AV_ERR;
        if (block > highest)
            highest = block;
    }
    out->block_number = highest;
    out->has_block_number = highest > 0;
    if (wait_mode == AV_WAIT_INDEXED && highest > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (long) (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        int left = max_wait - (int) (elapsed < max_wait ? elapsed : max_wait);
        if (av_collection_wait_block(col, highest, left > 0 ? left : 1, e) != AV_OK)
            return AV_ERR;
    }
    return AV_OK;
}
