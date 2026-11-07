/*
 * write/writer.c: Managed upsert-by-id writer, delete and extend operations, and unsigned prepare.
 */
#include "write/writer.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "postgres.h"
#include "varatt.h"
#include "vector.h"

#include "arkiv/abi.h"
#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "arkiv/tx.h"
#include "codec/base64.h"
#include "codec/hex.h"
#include "codec/text.h"
#include "codec/vectorbytes.h"
#include "codec/vtype.h"
#include "core/coldef.h"
#include "filter/value.h"
#include "store/schema.h"

struct av_writer {
    av_writer_config cfg;
    av_signer *signer;
    av_rpc *rpc;
    av_store *store;
    av_registry *reg;
    av_nonce_tracker *nonce_tracker;
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
        w->cfg.gas_limit = 500000;
    if (w->cfg.max_fee_per_gas == 0)
        w->cfg.max_fee_per_gas = 20000000000ULL; /* 20 gwei */
    if (w->cfg.max_priority_fee_per_gas == 0)
        w->cfg.max_priority_fee_per_gas = 1500000000ULL; /* 1.5 gwei */

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

/* Helper to broadcast a batch of transactions and wait for receipts */

/* Fallback gas limit when eth_estimateGas is unavailable: the old 50k per op was too low (a create
 * costs about 110k on Rabdi), so 10-entity batches ran out of gas and reverted. */
#define AV_GAS_PER_OP_FALLBACK 130000ULL

/*
 * Ask the chain what the transaction needs (eth_estimateGas) and add a 30% margin.
 * Returns `fallback` when the call fails, so a flaky RPC never blocks a write.
 */
static uint64_t estimate_gas_limit(av_writer *w, const uint8_t from[20], const uint8_t *data, size_t data_len,
                                   av_arena *arena, uint64_t fallback)
{
    if (w->rpc == NULL || from == NULL)
        return fallback;
    static const char hexd[] = "0123456789abcdef";
    size_t cap = 160 + data_len * 2;
    char *params = av_arena_alloc(arena, cap, 0);
    if (params == NULL)
        return fallback;
    size_t n = 0;
    n += (size_t) snprintf(params + n, cap - n, "[{\"from\":\"0x");
    for (size_t i = 0; i < 20; i++) {
        params[n++] = hexd[from[i] >> 4];
        params[n++] = hexd[from[i] & 15];
    }
    n += (size_t) snprintf(params + n, cap - n, "\",\"to\":\"0x");
    for (size_t i = 0; i < 20; i++) {
        params[n++] = hexd[w->cfg.contract_addr[i] >> 4];
        params[n++] = hexd[w->cfg.contract_addr[i] & 15];
    }
    n += (size_t) snprintf(params + n, cap - n, "\",\"data\":\"0x");
    for (size_t i = 0; i < data_len; i++) {
        params[n++] = hexd[data[i] >> 4];
        params[n++] = hexd[data[i] & 15];
    }
    snprintf(params + n, cap - n, "\"}]");

    yyjson_doc *doc = NULL;
    yyjson_val *res = NULL;
    int code = 0;
    av_err ee;
    av_err_clear(&ee);
    uint64_t out = fallback;
    if (av_rpc_call(w->rpc, "eth_estimateGas", params, &doc, &res, &code, &ee) == AV_OK && res != NULL) {
        const char *hex = yyjson_get_str(res);
        if (hex != NULL) {
            char *end = NULL;
            unsigned long long est = strtoull(hex, &end, 16);
            if (end != hex && est > 0 && est < 30000000ULL) {
                out = (uint64_t) est + (uint64_t) est * 3 / 10 + 21000;
            }
        }
    }
    if (doc != NULL)
        yyjson_doc_free(doc);
    return out;
}

static av_status broadcast_and_wait(av_writer *w, av_collection *col, av_op_batch *batches, size_t n_batches,
                                    av_write_wait_mode wait_mode, int timeout_ms, av_arena *arena,
                                    char (**out_hashes)[67], size_t *out_n_hashes, uint64_t *out_block_number,
                                    bool *has_block, av_err *e)
{
    *out_n_hashes = 0;
    *has_block = false;
    *out_block_number = 0;

    if (n_batches == 0)
        return AV_OK;

    char (*tx_hashes)[67] = av_arena_alloc(arena, n_batches * sizeof(*tx_hashes), 0);
    if (tx_hashes == NULL) {
        av_err_set(e, "internal", "out of memory allocating tx hashes");
        return AV_ERR;
    }

    uint64_t highest_block = 0;

    for (size_t i = 0; i < n_batches; i++) {
        uint64_t nonce = 0;
        if (av_nonce_tracker_next(w->nonce_tracker, &nonce, e) != AV_OK)
            return AV_ERR;

        av_tx_desc desc;
        memset(&desc, 0, sizeof(desc));
        desc.chain_id = w->cfg.chain_id;
        desc.nonce = nonce;
        desc.max_priority_fee_per_gas = w->cfg.max_priority_fee_per_gas;
        desc.max_fee_per_gas = w->cfg.max_fee_per_gas;
        desc.gas_limit = estimate_gas_limit(w, av_signer_get_address(w->signer), batches[i].calldata,
                                            batches[i].calldata_len, arena,
                                            w->cfg.gas_limit + batches[i].n_ops * AV_GAS_PER_OP_FALLBACK);
        memcpy(desc.to, w->cfg.contract_addr, 20);
        desc.data = batches[i].calldata;
        desc.data_len = batches[i].calldata_len;

        uint8_t *raw_tx = NULL;
        size_t raw_len = 0;
        uint8_t tx_hash[32];
        if (av_tx_sign_and_encode(&desc, w->signer, arena, &raw_tx, &raw_len, tx_hash, e) != AV_OK)
            return AV_ERR;

        av_key_to_hex(tx_hash, tx_hashes[i]);

        if (w->rpc != NULL) {
            char *raw_hex = av_arena_alloc(arena, raw_len * 2 + 3, 0);
            if (raw_hex == NULL) {
                av_err_set(e, "internal", "out of memory for raw hex");
                return AV_ERR;
            }
            raw_hex[0] = '0';
            raw_hex[1] = 'x';
            static const char hex_digits[] = "0123456789abcdef";
            for (size_t b = 0; b < raw_len; b++) {
                raw_hex[2 + b * 2] = hex_digits[(raw_tx[b] >> 4) & 0x0f];
                raw_hex[2 + b * 2 + 1] = hex_digits[raw_tx[b] & 0x0f];
            }
            raw_hex[2 + raw_len * 2] = '\0';

            char params_json[raw_len * 2 + 16];
            snprintf(params_json, sizeof(params_json), "[\"%s\"]", raw_hex);

            yyjson_doc *resp_doc = NULL;
            yyjson_val *resp_val = NULL;
            int rpc_code = 0;
            av_status st = av_rpc_call(w->rpc, "eth_sendRawTransaction", params_json, &resp_doc, &resp_val,
                                       &rpc_code, e);
            if (st != AV_OK)
                return AV_ERR;

            if (resp_doc != NULL) {
                yyjson_val *root = yyjson_doc_get_root(resp_doc);
                yyjson_val *err_val = yyjson_obj_get(root, "error");
                if (err_val != NULL) {
                    yyjson_val *msg = yyjson_obj_get(err_val, "message");
                    const char *mstr = yyjson_get_str(msg);
                    av_err_set(e, "tx_reverted", "transaction rejected: %s", mstr ? mstr : "unknown error");
                    yyjson_doc_free(resp_doc);
                    return AV_ERR;
                }
                yyjson_doc_free(resp_doc);
            }

            /* Wait for receipt if requested */
            if (wait_mode != AV_WAIT_NONE) {
                int elapsed = 0;
                int sleep_step = 50;
                int max_wait = timeout_ms > 0 ? timeout_ms : 60000;
                bool got_receipt = false;

                while (elapsed < max_wait) {
                    char qparams[128];
                    snprintf(qparams, sizeof(qparams), "[\"%s\"]", tx_hashes[i]);
                    yyjson_doc *rc_doc = NULL;
                    yyjson_val *rc_val = NULL;
                    int rc_code = 0;
                    if (av_rpc_call(w->rpc, "eth_getTransactionReceipt", qparams, &rc_doc, &rc_val, &rc_code,
                                    e) == AV_OK &&
                        rc_doc != NULL) {
                        if (rc_val != NULL && !yyjson_is_null(rc_val)) {
                            yyjson_val *status_val = yyjson_obj_get(rc_val, "status");
                            const char *st_str = yyjson_get_str(status_val);
                            if (st_str != NULL && strcmp(st_str, "0x0") == 0) {
                                yyjson_doc_free(rc_doc);
                                av_err_set(e, "tx_reverted", "transaction reverted on-chain");
                                return AV_ERR;
                            }
                            yyjson_val *bn_val = yyjson_obj_get(rc_val, "blockNumber");
                            const char *bn_str = yyjson_get_str(bn_val);
                            if (bn_str != NULL) {
                                uint64_t bnum = 0;
                                if (av_qty_parse(bn_str, &bnum)) {
                                    if (bnum > highest_block)
                                        highest_block = bnum;
                                }
                            }
                            got_receipt = true;
                            yyjson_doc_free(rc_doc);
                            break;
                        }
                        yyjson_doc_free(rc_doc);
                    }
                    usleep((useconds_t) (sleep_step * 1000));
                    elapsed += sleep_step;
                }

                if (!got_receipt) {
                    av_err_set(e, "consistency_timeout", "timeout waiting for transaction receipt");
                    return AV_ERR;
                }
            }
        }
    }

    *out_hashes = tx_hashes;
    *out_n_hashes = n_batches;

    if (wait_mode != AV_WAIT_NONE && highest_block > 0) {
        *out_block_number = highest_block;
        *has_block = true;

        if (wait_mode == AV_WAIT_INDEXED && col != NULL) {
            if (av_collection_wait_block(col, highest_block, timeout_ms, e) != AV_OK)
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
    size_t create_counter = 0;

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

            av_abi_predict_entity_key(w->cfg.chain_id, w->cfg.contract_addr, signer_addr, create_counter,
                                      salt, results[i].key);
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

    if (broadcast_and_wait(w, col, batches, n_batches, wait_mode, timeout_ms, arena, &tx_hashes, &n_tx_hashes,
                           &block_number, &has_block, e) != AV_OK) {
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
    if (broadcast_and_wait(w, col, batches, n_batches, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes, &n_hashes,
                           &bnum, &has_b, e) != AV_OK)
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
    if (broadcast_and_wait(w, col, batches, n_batches, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes, &n_hashes,
                           &bnum, &has_b, e) != AV_OK)
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
    if (broadcast_and_wait(w, col, batches, n_batches, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes, &n_hashes,
                           &bnum, &has_b, e) != AV_OK)
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
    if (broadcast_and_wait(w, col, batches, n_batches, AV_WAIT_RECEIPT, 60000, arena, &tx_hashes, &n_hashes,
                           &bnum, &has_b, e) != AV_OK)
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
    size_t create_counter = 0;

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
        txs[b].nonce = b;
        txs[b].gas_estimate = w->cfg.gas_limit + batches[b].n_ops * AV_GAS_PER_OP_FALLBACK;

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
