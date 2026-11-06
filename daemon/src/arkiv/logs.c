/*
 * arkiv/logs.c: decode Arkiv contract event logs. See logs.h.
 */
#include "arkiv/logs.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "yyjson.h"

static const struct {
    const char *sig;
    const char *topic0_hex; /* documented constant (events-and-logs.md §3) */
} k_events[AV_EV_KINDS] = {
    {"EntityCreated(bytes32,address,uint64,uint8)",
     "0xb282d7c494b8899aa8015cd07be621530beb03409eb8c5e8fdc1411ba64356a5"},
    {"EntityPatched(bytes32,address)", "0xe986ee220b2d7947a009d9389d8400b2ac82d3b430a9f72700a0975d3d5a34fe"},
    {"EntityDeleted(bytes32,address)", "0x4059b76c47e1ecc40ba88e649b654f716515eb358c3ce83c803820e3b3130cc3"},
    {"ExpiryExtended(bytes32,address,uint64)",
     "0x10dc3526654c9ba4e2370b1bfcbc0de087d141a5694b3d3305d37df7e6705d4b"},
    {"OwnershipTransferred(bytes32,address,address)",
     "0x0b659dccc8eb950324170e8d9598af5ee04ee070883eb28651a96788721fbf83"},
};

static uint8_t g_topic0[AV_EV_KINDS][32];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void topics_init(void)
{
    for (int i = 0; i < AV_EV_KINDS; i++) {
        uint8_t want[32];
        av_keccak256(k_events[i].sig, strlen(k_events[i].sig), g_topic0[i]);
        if (!av_hex_to_buf(k_events[i].topic0_hex, strlen(k_events[i].topic0_hex), want, 32, NULL) ||
            memcmp(want, g_topic0[i], 32) != 0) {
            av_log(AV_LOG_ERROR, "arkiv", "keccak256(%s) does not match the documented topic0",
                   k_events[i].sig);
            abort();
        }
    }
}

const uint8_t *av_event_topic0(av_event_kind k)
{
    if (k < AV_EV_CREATED || k > AV_EV_TRANSFERRED)
        return NULL;
    pthread_once(&g_once, topics_init);
    return g_topic0[(int) k - 1];
}

static av_status bad(av_err *e, const char *what)
{
    av_err_set(e, "arkiv_rpc_error", "malformed log entry: %s", what);
    return AV_ERR;
}

/* Hex string field of exactly nbytes bytes. */
static bool hex_exact(yyjson_val *v, uint8_t *out, size_t nbytes)
{
    const char *s = yyjson_get_str(v);
    size_t n = 0;
    return s != NULL && yyjson_get_len(v) == 2 + 2 * nbytes &&
           av_hex_to_buf(s, yyjson_get_len(v), out, nbytes, &n) && n == nbytes;
}

/* An address topic: 12 zero bytes then the 20-byte address. */
static bool topic_addr(yyjson_val *v, uint8_t out[20])
{
    uint8_t w[32];
    static const uint8_t zero12[12] = {0};
    if (!hex_exact(v, w, 32) || memcmp(w, zero12, 12) != 0)
        return false;
    memcpy(out, w + 12, 20);
    return true;
}

/* A uint word whose value must fit in max_bytes; returns the value. */
static bool word_uint(const uint8_t w[32], size_t max_bytes, uint64_t *out)
{
    for (size_t i = 0; i < 32 - max_bytes; i++)
        if (w[i] != 0)
            return false;
    uint64_t v = 0;
    for (size_t i = 24; i < 32; i++)
        v = (v << 8) | w[i];
    *out = v;
    return true;
}

static bool qty_field(yyjson_val *o, const char *name, uint64_t *out)
{
    yyjson_val *v = yyjson_obj_get(o, name);
    const char *s = yyjson_get_str(v);
    return s != NULL && strlen(s) == yyjson_get_len(v) && av_qty_parse(s, out);
}

av_status av_event_parse(yyjson_val *log, av_event *out, bool *known, av_err *e)
{
    uint8_t t0[32];
    yyjson_val *topics = yyjson_obj_get(log, "topics");
    *known = false;
    if (!yyjson_is_obj(log))
        return bad(e, "not an object");
    if (!yyjson_is_arr(topics) || yyjson_arr_size(topics) == 0)
        return bad(e, "topics missing");
    if (!hex_exact(yyjson_arr_get_first(topics), t0, 32))
        return bad(e, "topic0 not 32-byte hex");

    int kind = 0;
    for (int k = AV_EV_CREATED; k <= AV_EV_TRANSFERRED; k++)
        if (memcmp(av_event_topic0((av_event_kind) k), t0, 32) == 0)
            kind = k;
    if (kind == 0)
        return AV_OK; /* not ours */

    size_t want_topics = kind == AV_EV_TRANSFERRED ? 4 : 3;
    size_t want_data = kind == AV_EV_CREATED ? 64 : (kind == AV_EV_EXTENDED ? 32 : 0);
    if (yyjson_arr_size(topics) != want_topics)
        return bad(e, "wrong topic count");

    memset(out, 0, sizeof *out);
    out->kind = (av_event_kind) kind;
    if (!hex_exact(yyjson_arr_get(topics, 1), out->key, 32))
        return bad(e, "entityKey topic");
    if (kind == AV_EV_TRANSFERRED) {
        if (!topic_addr(yyjson_arr_get(topics, 2), out->prev_owner) ||
            !topic_addr(yyjson_arr_get(topics, 3), out->new_owner))
            return bad(e, "owner topics");
    } else if (!topic_addr(yyjson_arr_get(topics, 2), out->owner)) {
        return bad(e, "owner topic");
    }

    yyjson_val *dv = yyjson_obj_get(log, "data");
    const char *ds = yyjson_get_str(dv);
    uint8_t data[64];
    size_t dn = 0;
    if (ds == NULL || yyjson_get_len(dv) > 2 + 2 * sizeof data ||
        !av_hex_to_buf(ds, yyjson_get_len(dv), data, sizeof data, &dn) || dn != want_data)
        return bad(e, "data length");
    if (kind == AV_EV_CREATED || kind == AV_EV_EXTENDED) {
        uint64_t flags = 0;
        if (!word_uint(data, 8, &out->expires_at))
            return bad(e, "expiresAt exceeds uint64");
        if (kind == AV_EV_CREATED) {
            if (!word_uint(data + 32, 1, &flags))
                return bad(e, "creationFlags exceeds uint8");
            out->flags = (uint32_t) flags;
        }
    }

    uint64_t log_index = 0;
    if (!qty_field(log, "blockNumber", &out->block) || !qty_field(log, "logIndex", &log_index) ||
        log_index > UINT32_MAX)
        return bad(e, "blockNumber/logIndex");
    out->log_index = (uint32_t) log_index;
    if (!hex_exact(yyjson_obj_get(log, "blockHash"), out->block_hash, 32) ||
        !hex_exact(yyjson_obj_get(log, "transactionHash"), out->tx_hash, 32))
        return bad(e, "blockHash/transactionHash");
    yyjson_val *rv = yyjson_obj_get(log, "removed");
    out->removed = yyjson_is_bool(rv) && yyjson_get_bool(rv);
    *known = true;
    return AV_OK;
}

av_status av_events_parse(yyjson_val *arr, av_arena *a, av_event **evs, size_t *n, av_err *e)
{
    *evs = NULL;
    *n = 0;
    if (!yyjson_is_arr(arr))
        return bad(e, "eth_getLogs result is not an array");
    size_t cnt = yyjson_arr_size(arr);
    if (cnt == 0)
        return AV_OK;
    av_event *out = av_arena_alloc(a, cnt * sizeof *out, 0);
    if (out == NULL) {
        av_err_set(e, "internal", "out of memory parsing logs");
        return AV_ERR;
    }
    size_t idx, max, w = 0;
    yyjson_val *lv;
    yyjson_arr_foreach(arr, idx, max, lv)
    {
        bool known = false;
        if (av_event_parse(lv, &out[w], &known, e) != AV_OK)
            return AV_ERR;
        if (known)
            w++;
    }
    *evs = out;
    *n = w;
    return AV_OK;
}
