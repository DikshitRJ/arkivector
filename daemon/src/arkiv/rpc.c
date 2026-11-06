/*
 * arkiv/rpc.c: blocking JSON-RPC client. See rpc.h.
 */
#include "arkiv/rpc.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arkiv/budget.h"
#include "arkiv/query_builder.h"
#include "yyjson.h"

#define AV_RPC_DEFAULT_TIMEOUT_MS 10000
#define AV_RPC_MAX_RETRIES        10
#define AV_RPC_BAD_MS             3000
#define AV_RPC_BACKOFF_BASE_MS    100
#define AV_RPC_BACKOFF_MAX_MS     5000
#define AV_RPC_MAX_URLS           16

struct av_rpc {
    char **urls;
    size_t n_urls;
    char *access_key;
    char *contract;
    int timeout_ms;
    int max_retries;
    uint64_t chain_id;
    av_budget *budget;
    pthread_mutex_t mu;  /* guards bad_until */
    uint64_t *bad_until; /* monotonic ms; endpoint skipped while now < bad_until */
    atomic_uint_fast64_t next_id;
    atomic_uint_fast64_t attempts;
};

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static int g_curl_ok;

static void curl_init_once(void)
{
    g_curl_ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000U + (uint64_t) ts.tv_nsec / 1000000U;
}

static void sleep_ms(uint64_t ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t) (ms / 1000U);
    ts.tv_nsec = (long) ((ms % 1000U) * 1000000U);
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ lifecycle */

av_rpc *av_rpc_create(const av_rpc_config *cfg, av_err *e)
{
    if (cfg == NULL || cfg->urls == NULL || cfg->n_urls == 0 || cfg->n_urls > AV_RPC_MAX_URLS) {
        av_err_set(e, "invalid_argument", "rpc: 1..%d endpoint urls are required", AV_RPC_MAX_URLS);
        return NULL;
    }
    for (size_t i = 0; i < cfg->n_urls; i++) {
        const char *u = cfg->urls[i];
        if (u == NULL || (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0)) {
            av_err_set(e, "invalid_argument", "rpc: endpoint #%zu is not an http(s) url", i);
            return NULL;
        }
    }
    pthread_once(&g_curl_once, curl_init_once);
    if (!g_curl_ok) {
        av_err_set(e, "internal", "rpc: curl_global_init failed");
        return NULL;
    }
    av_rpc *r = calloc(1, sizeof *r);
    if (r == NULL) {
        av_err_set(e, "internal", "rpc: out of memory");
        return NULL;
    }
    r->urls = (char **) calloc(cfg->n_urls, sizeof *r->urls);
    r->bad_until = calloc(cfg->n_urls, sizeof *r->bad_until);
    if (r->urls == NULL || r->bad_until == NULL)
        goto oom;
    r->n_urls = cfg->n_urls;
    for (size_t i = 0; i < cfg->n_urls; i++) {
        r->urls[i] = strdup(cfg->urls[i]);
        if (r->urls[i] == NULL)
            goto oom;
    }
    if (cfg->access_key != NULL && cfg->access_key[0] != '\0') {
        r->access_key = strdup(cfg->access_key);
        if (r->access_key == NULL)
            goto oom;
    }
    r->contract = strdup(cfg->contract != NULL ? cfg->contract : AV_RPC_DEFAULT_CONTRACT);
    if (r->contract == NULL)
        goto oom;
    uint8_t probe[20];
    if (!av_addr_from_hex(r->contract, probe)) {
        av_err_set(e, "invalid_argument", "rpc: contract is not a 20-byte hex address");
        av_rpc_destroy(r);
        return NULL;
    }
    r->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : AV_RPC_DEFAULT_TIMEOUT_MS;
    r->max_retries = cfg->max_retries < 0
                         ? 0
                         : (cfg->max_retries > AV_RPC_MAX_RETRIES ? AV_RPC_MAX_RETRIES : cfg->max_retries);
    r->chain_id = cfg->chain_id;
    r->budget = av_budget_create(cfg->budget_per_sec);
    if (r->budget == NULL)
        goto oom;
    pthread_mutex_init(&r->mu, NULL);
    atomic_init(&r->next_id, 1);
    atomic_init(&r->attempts, 0);
    return r;
oom:
    av_err_set(e, "internal", "rpc: out of memory");
    av_rpc_destroy(r);
    return NULL;
}

void av_rpc_destroy(av_rpc *r)
{
    if (r == NULL)
        return;
    if (r->urls != NULL)
        for (size_t i = 0; i < r->n_urls; i++)
            free(r->urls[i]);
    free((void *) r->urls);
    free(r->bad_until);
    free(r->access_key);
    free(r->contract);
    av_budget_destroy(r->budget);
    if (r->budget != NULL)
        pthread_mutex_destroy(&r->mu);
    free(r);
}

uint64_t av_rpc_attempts(av_rpc *r)
{
    return (uint64_t) atomic_load(&r->attempts);
}

/* ------------------------------------------------------------------ one HTTP attempt */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
    bool too_big;
} rbuf;

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    rbuf *b = (rbuf *) ud;
    size_t n = sz * nm;
    if (sz != 0 && n / sz != nm)
        return 0;
    if (n > AV_RPC_MAX_RESPONSE - b->len) {
        b->too_big = true;
        return 0;
    }
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap == 0 ? 4096 : b->cap;
        while (nc < b->len + n + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (np == NULL)
            return 0;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, ptr, n);
    b->len += n;
    b->p[b->len] = '\0';
    return n;
}

typedef enum { OUT_OK, OUT_TRANSPORT, OUT_HTTP_RETRY, OUT_FATAL } outcome;

static size_t pick_endpoint(av_rpc *r, bool *healthy)
{
    uint64_t now = mono_ms();
    size_t best = 0;
    pthread_mutex_lock(&r->mu);
    for (size_t i = 0; i < r->n_urls; i++) {
        if (r->bad_until[i] <= now) {
            pthread_mutex_unlock(&r->mu);
            *healthy = true;
            return i;
        }
        if (r->bad_until[i] < r->bad_until[best])
            best = i;
    }
    pthread_mutex_unlock(&r->mu);
    *healthy = false;
    return best;
}

static void mark_bad(av_rpc *r, size_t i)
{
    pthread_mutex_lock(&r->mu);
    r->bad_until[i] = mono_ms() + AV_RPC_BAD_MS;
    pthread_mutex_unlock(&r->mu);
}

static void mark_good(av_rpc *r, size_t i)
{
    pthread_mutex_lock(&r->mu);
    r->bad_until[i] = 0;
    pthread_mutex_unlock(&r->mu);
}

/* detail receives a short, secret-free description of a failure. */
static outcome http_attempt(av_rpc *r, size_t ep, const char *body, size_t body_len, rbuf *out, char *detail,
                            size_t detail_cap)
{
    CURL *c = curl_easy_init();
    struct curl_slist *hdrs = NULL;
    outcome res = OUT_TRANSPORT;
    long status = 0;

    if (c == NULL) {
        snprintf(detail, detail_cap, "curl_easy_init failed");
        return OUT_FATAL;
    }
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (r->access_key != NULL) {
        size_t n = strlen(r->access_key) + 16;
        char *h = malloc(n);
        if (h == NULL) {
            curl_slist_free_all(hdrs);
            curl_easy_cleanup(c);
            snprintf(detail, detail_cap, "out of memory");
            return OUT_FATAL;
        }
        snprintf(h, n, "X-API-KEY: %s", r->access_key);
        hdrs = curl_slist_append(hdrs, h);
        free(h);
    }
    curl_easy_setopt(c, CURLOPT_URL, r->urls[ep]);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) body_len);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) r->timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long) r->timeout_ms);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    atomic_fetch_add(&r->attempts, 1);

    CURLcode cc = curl_easy_perform(c);
    if (cc != CURLE_OK) {
        if (out->too_big)
            snprintf(detail, detail_cap, "response exceeds %zu bytes", (size_t) AV_RPC_MAX_RESPONSE);
        else
            snprintf(detail, detail_cap, "transport error: %s", curl_easy_strerror(cc));
        res = out->too_big ? OUT_FATAL : OUT_TRANSPORT;
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        if (status == 200) {
            res = OUT_OK;
        } else if (status == 429 || status >= 500) {
            snprintf(detail, detail_cap, "HTTP %ld", status);
            res = OUT_HTTP_RETRY;
        } else {
            snprintf(detail, detail_cap, "HTTP %ld", status);
            res = OUT_FATAL;
        }
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return res;
}

/* ------------------------------------------------------------------ av_rpc_call */

static uint64_t jitter(uint64_t seed, uint64_t ms)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return ms / 2 + (ms / 2 > 0 ? seed % (ms / 2 + 1) : 0);
}

static bool method_ok(const char *m)
{
    if (m == NULL || m[0] == '\0' || strlen(m) > 64)
        return false;
    for (const char *p = m; *p != '\0'; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    return true;
}

/* Interpret a 200 response body. Returns OUT_OK with the doc and result set, OUT_TRANSPORT for an
   unparsable body (retryable), OUT_FATAL for protocol violations and JSON-RPC errors. */
static outcome interpret(const rbuf *b, uint64_t id, const char *method, yyjson_doc **doc,
                         yyjson_val **result, int *rpc_code, av_err *e)
{
    yyjson_doc *d = yyjson_read(b->p != NULL ? b->p : "", b->len, 0);
    if (d == NULL) {
        av_err_set(e, "arkiv_rpc_error", "%s: response is not valid JSON", method);
        return OUT_TRANSPORT;
    }
    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_val *vid = yyjson_obj_get(root, "id");
    if (!yyjson_is_obj(root) || !yyjson_is_uint(vid) || yyjson_get_uint(vid) != id) {
        yyjson_doc_free(d);
        av_err_set(e, "arkiv_rpc_error", "%s: response id does not match the request", method);
        return OUT_FATAL;
    }
    yyjson_val *err = yyjson_obj_get(root, "error");
    if (err != NULL && !yyjson_is_null(err)) {
        yyjson_val *vc = yyjson_obj_get(err, "code");
        yyjson_val *vm = yyjson_obj_get(err, "message");
        int64_t code = yyjson_is_int(vc) ? yyjson_get_sint(vc) : -1;
        *rpc_code = (code >= INT32_MIN && code <= INT32_MAX) ? (int) code : -1;
        av_err_set(e, "arkiv_rpc_error", "%s: JSON-RPC error %d: %.200s", method, *rpc_code,
                   yyjson_get_str(vm) != NULL ? yyjson_get_str(vm) : "(no message)");
        yyjson_doc_free(d);
        return OUT_FATAL;
    }
    yyjson_val *res = yyjson_obj_get(root, "result");
    if (res == NULL) {
        yyjson_doc_free(d);
        av_err_set(e, "arkiv_rpc_error", "%s: response has neither result nor error", method);
        return OUT_FATAL;
    }
    *doc = d;
    *result = res;
    return OUT_OK;
}

av_status av_rpc_call(av_rpc *r, const char *method, const char *params_json, yyjson_doc **doc,
                      yyjson_val **result, int *rpc_code, av_err *e)
{
    int dummy = 0;
    if (rpc_code == NULL)
        rpc_code = &dummy;
    *rpc_code = 0;
    *doc = NULL;
    *result = NULL;
    if (!method_ok(method)) {
        av_err_set(e, "invalid_argument", "rpc: invalid method name");
        return AV_ERR;
    }
    if (params_json == NULL)
        params_json = "[]";
    yyjson_doc *pd = yyjson_read(params_json, strlen(params_json), 0);
    if (pd == NULL || !yyjson_is_arr(yyjson_doc_get_root(pd))) {
        yyjson_doc_free(pd);
        av_err_set(e, "invalid_argument", "rpc: params must be a JSON array");
        return AV_ERR;
    }
    yyjson_doc_free(pd);

    uint64_t id = (uint64_t) atomic_fetch_add(&r->next_id, 1);
    size_t blen = strlen(method) + strlen(params_json) + 80;
    char *body = malloc(blen);
    if (body == NULL) {
        av_err_set(e, "internal", "rpc: out of memory");
        return AV_ERR;
    }
    int bn = snprintf(body, blen, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%s\",\"params\":%s}",
                      (unsigned long long) id, method, params_json);
    if (bn < 0 || (size_t) bn >= blen) {
        free(body);
        av_err_set(e, "internal", "rpc: request build failed");
        return AV_ERR;
    }

    av_budget_acquire(r->budget, av_budget_cost(method));

    uint64_t seed = mono_ms() * 2654435761ULL + id + 0x9E3779B97F4A7C15ULL;
    av_status st = AV_ERR;
    for (int attempt = 0;; attempt++) {
        bool healthy = false;
        size_t ep = pick_endpoint(r, &healthy);
        rbuf rb = {NULL, 0, 0, false};
        char detail[160] = "";
        outcome o = http_attempt(r, ep, body, (size_t) bn, &rb, detail, sizeof detail);

        if (o == OUT_OK) {
            mark_good(r, ep);
            o = interpret(&rb, id, method, doc, result, rpc_code, e);
            if (o == OUT_OK)
                st = AV_OK;
        } else {
            if (o == OUT_TRANSPORT || o == OUT_HTTP_RETRY)
                mark_bad(r, ep);
            av_err_set(e, "arkiv_rpc_error", "%s: endpoint #%zu: %s", method, ep, detail);
        }
        free(rb.p);
        if (st == AV_OK || o == OUT_FATAL || attempt >= r->max_retries)
            break;

        bool next_healthy = false;
        size_t next = pick_endpoint(r, &next_healthy);
        if (!(next_healthy && next != ep)) {
            uint64_t base = (uint64_t) AV_RPC_BACKOFF_BASE_MS << (attempt < 6 ? attempt : 6);
            if (base > AV_RPC_BACKOFF_MAX_MS)
                base = AV_RPC_BACKOFF_MAX_MS;
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            sleep_ms(jitter(seed, base));
        }
        av_log(AV_LOG_DEBUG, "arkiv", "retrying %s (attempt %d): %s", method, attempt + 2,
               e != NULL ? e->msg : "");
    }
    free(body);
    return st;
}

/* ------------------------------------------------------------------ helpers */

static av_status proto_err(av_err *e, const char *method, const char *what)
{
    av_err_set(e, "arkiv_rpc_error", "%s: malformed result (%s)", method, what);
    return AV_ERR;
}

static bool val_u64(yyjson_val *v, uint64_t *out)
{
    if (yyjson_is_uint(v)) {
        *out = yyjson_get_uint(v);
        return true;
    }
    const char *s = yyjson_get_str(v);
    return s != NULL && strlen(s) == yyjson_get_len(v) && av_qty_parse(s, out);
}

static bool val_hex32(yyjson_val *v, uint8_t out[32])
{
    const char *s = yyjson_get_str(v);
    size_t n = 0;
    return s != NULL && yyjson_get_len(v) == 66 && av_hex_to_buf(s, 66, out, 32, &n) && n == 32;
}

av_status av_rpc_block_number(av_rpc *r, uint64_t *out, av_err *e)
{
    yyjson_doc *d;
    yyjson_val *res;
    int code;
    if (av_rpc_call(r, "eth_blockNumber", "[]", &d, &res, &code, e) != AV_OK)
        return AV_ERR;
    bool ok = yyjson_is_str(res) && val_u64(res, out);
    yyjson_doc_free(d);
    return ok ? AV_OK : proto_err(e, "eth_blockNumber", "not a hex quantity");
}

av_status av_rpc_get_block(av_rpc *r, uint64_t number, av_block *out, av_err *e)
{
    char params[64];
    yyjson_doc *d;
    yyjson_val *res;
    int code;
    snprintf(params, sizeof params, "[\"0x%llx\",false]", (unsigned long long) number);
    if (av_rpc_call(r, "eth_getBlockByNumber", params, &d, &res, &code, e) != AV_OK)
        return AV_ERR;
    if (yyjson_is_null(res)) {
        yyjson_doc_free(d);
        av_err_set(e, "arkiv_rpc_error", "block %llu not found", (unsigned long long) number);
        return AV_ERR;
    }
    memset(out, 0, sizeof *out);
    bool ok = yyjson_is_obj(res) && val_u64(yyjson_obj_get(res, "number"), &out->number) &&
              val_hex32(yyjson_obj_get(res, "hash"), out->hash) &&
              val_hex32(yyjson_obj_get(res, "parentHash"), out->parent_hash) &&
              val_u64(yyjson_obj_get(res, "timestamp"), &out->timestamp);
    yyjson_doc_free(d);
    if (ok && out->number != number)
        ok = false;
    return ok ? AV_OK : proto_err(e, "eth_getBlockByNumber", "block fields");
}

av_status av_rpc_block_timing(av_rpc *r, av_block_timing *out, av_err *e)
{
    yyjson_doc *d;
    yyjson_val *res;
    int code;
    if (av_rpc_call(r, "arkiv_getBlockTiming", "[]", &d, &res, &code, e) != AV_OK)
        return AV_ERR;
    memset(out, 0, sizeof *out);
    bool ok = yyjson_is_obj(res) && val_u64(yyjson_obj_get(res, "current_block"), &out->current_block) &&
              val_u64(yyjson_obj_get(res, "current_block_time"), &out->current_block_time) &&
              val_u64(yyjson_obj_get(res, "duration"), &out->block_duration);
    yyjson_doc_free(d);
    return ok ? AV_OK : proto_err(e, "arkiv_getBlockTiming", "timing fields");
}

/* Serialise and free a mutable document. */
static char *mut_json(yyjson_mut_doc *md)
{
    char *s = yyjson_mut_write(md, 0, NULL);
    yyjson_mut_doc_free(md);
    return s;
}

static av_status oom(av_err *e)
{
    av_err_set(e, "internal", "rpc: out of memory");
    return AV_ERR;
}

av_status av_rpc_query(av_rpc *r, const char *query, const char *cursor, int limit, bool key_only,
                       av_arena *a, av_query_page *out, int *rpc_code, av_err *e)
{
    uint8_t cur_key[32];
    char qbuf[AV_QB_QUERY_MAX];
    char hexlimit[16];
    int dummy = 0;
    if (rpc_code == NULL)
        rpc_code = &dummy;
    *rpc_code = 0;
    memset(out, 0, sizeof *out);
    if (limit < 1 || limit > 100) {
        av_err_set(e, "invalid_argument", "rpc: query limit must be 1..100");
        return AV_ERR;
    }
    if (cursor != NULL && !av_key_from_hex(cursor, cur_key)) {
        av_err_set(e, "invalid_argument", "rpc: cursor is not a 32-byte hex key");
        return AV_ERR;
    }
    if (av_qb_paged(qbuf, sizeof qbuf, query, cursor != NULL ? cur_key : NULL, e) != AV_OK)
        return AV_ERR;

    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL)
        return oom(e);
    yyjson_mut_val *params = yyjson_mut_arr(md);
    yyjson_mut_val *opts = yyjson_mut_obj(md);
    yyjson_mut_val *sel = yyjson_mut_obj(md);
    snprintf(hexlimit, sizeof hexlimit, "0x%x", (unsigned) limit);
    bool built = params != NULL && opts != NULL && sel != NULL;
    if (built) {
        yyjson_mut_doc_set_root(md, params);
        built = yyjson_mut_arr_add_strcpy(md, params, qbuf) && yyjson_mut_obj_add_true(md, sel, "key") &&
                (key_only ||
                 (yyjson_mut_obj_add_true(md, sel, "owner") && yyjson_mut_obj_add_true(md, sel, "creator") &&
                  yyjson_mut_obj_add_true(md, sel, "createdAt") &&
                  yyjson_mut_obj_add_true(md, sel, "updatedAt") &&
                  yyjson_mut_obj_add_true(md, sel, "expiresAt") &&
                  yyjson_mut_obj_add_true(md, sel, "attributes"))) &&
                yyjson_mut_obj_add_val(md, opts, "select", sel) &&
                yyjson_mut_obj_add_strcpy(md, opts, "limit", hexlimit) &&
                yyjson_mut_arr_add_val(params, opts);
    }
    if (!built) {
        yyjson_mut_doc_free(md);
        return oom(e);
    }
    char *pj = mut_json(md);
    if (pj == NULL)
        return oom(e);

    yyjson_doc *d;
    yyjson_val *res;
    av_status st = av_rpc_call(r, "arkiv_query", pj, &d, &res, rpc_code, e);
    free(pj);
    if (st != AV_OK)
        return AV_ERR;

    yyjson_val *data = yyjson_obj_get(res, "data");
    size_t n = yyjson_is_arr(data) ? yyjson_arr_size(data) : 0;
    if (!yyjson_is_obj(res) || !yyjson_is_arr(data) || n > (size_t) limit ||
        !val_u64(yyjson_obj_get(res, "blockNumber"), &out->block)) {
        yyjson_doc_free(d);
        return proto_err(e, "arkiv_query", "data/blockNumber");
    }
    if (n > 0) {
        out->ents = av_arena_alloc(a, n * sizeof(av_entity), 0);
        if (out->ents == NULL) {
            yyjson_doc_free(d);
            return oom(e);
        }
    }
    size_t idx, max;
    yyjson_val *ev;
    yyjson_arr_foreach(data, idx, max, ev)
    {
        if (av_entity_from_json(ev, a, &out->ents[idx], e) != AV_OK) {
            yyjson_doc_free(d);
            return AV_ERR;
        }
        const uint8_t *prev = idx > 0 ? out->ents[idx - 1].key : (cursor != NULL ? cur_key : NULL);
        if (prev != NULL && memcmp(out->ents[idx].key, prev, 32) <= 0) {
            yyjson_doc_free(d);
            return proto_err(e, "arkiv_query", "keys not strictly ascending after the cursor");
        }
        out->n = idx + 1;
    }
    yyjson_doc_free(d);
    if (out->n == (size_t) limit) {
        char hex[67];
        av_key_to_hex(out->ents[out->n - 1].key, hex);
        out->next_cursor = av_arena_strdup(a, hex);
        if (out->next_cursor == NULL)
            return oom(e);
    }
    return AV_OK;
}

av_status av_rpc_get_entity(av_rpc *r, const uint8_t key[32], av_arena *a, av_entity *out, bool *found,
                            av_err *e)
{
    char params[96];
    char hex[67];
    yyjson_doc *d;
    yyjson_val *res;
    int code;
    *found = false;
    av_key_to_hex(key, hex);
    snprintf(params, sizeof params, "[\"%s\"]", hex);
    if (av_rpc_call(r, "arkiv_getEntity", params, &d, &res, &code, e) != AV_OK)
        return AV_ERR;
    if (yyjson_is_null(res)) {
        yyjson_doc_free(d);
        return AV_OK;
    }
    av_status st = av_entity_from_json(res, a, out, e);
    yyjson_doc_free(d);
    if (st != AV_OK)
        return AV_ERR;
    if (memcmp(out->key, key, 32) != 0)
        return proto_err(e, "arkiv_getEntity", "returned key differs from the requested key");
    *found = true;
    return AV_OK;
}

av_status av_rpc_get_logs(av_rpc *r, uint64_t from, uint64_t to, const char *topics_json, av_arena *a,
                          struct av_event **evs, size_t *n, int *rpc_code, av_err *e)
{
    *evs = NULL;
    *n = 0;
    if (from > to) {
        av_err_set(e, "invalid_argument", "rpc: eth_getLogs from > to");
        return AV_ERR;
    }
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL)
        return oom(e);
    yyjson_mut_val *params = yyjson_mut_arr(md);
    yyjson_mut_val *flt = yyjson_mut_obj(md);
    yyjson_mut_val *topics = NULL;
    char hf[24];
    char ht[24];
    snprintf(hf, sizeof hf, "0x%llx", (unsigned long long) from);
    snprintf(ht, sizeof ht, "0x%llx", (unsigned long long) to);
    if (topics_json != NULL) {
        yyjson_doc *td = yyjson_read(topics_json, strlen(topics_json), 0);
        if (td == NULL || !yyjson_is_arr(yyjson_doc_get_root(td))) {
            yyjson_doc_free(td);
            yyjson_mut_doc_free(md);
            av_err_set(e, "invalid_argument", "rpc: topics must be a JSON array");
            return AV_ERR;
        }
        topics = yyjson_val_mut_copy(md, yyjson_doc_get_root(td));
        yyjson_doc_free(td);
    } else if (params != NULL) {
        topics = yyjson_mut_arr(md);
        yyjson_mut_val *alt = yyjson_mut_arr(md);
        for (int k = AV_EV_CREATED; alt != NULL && k <= AV_EV_TRANSFERRED; k++) {
            char hex[67];
            av_key_to_hex(av_event_topic0((av_event_kind) k), hex);
            if (!yyjson_mut_arr_add_strcpy(md, alt, hex))
                alt = NULL;
        }
        if (topics == NULL || alt == NULL || !yyjson_mut_arr_add_val(topics, alt))
            topics = NULL;
    }
    if (params == NULL || flt == NULL || topics == NULL) {
        yyjson_mut_doc_free(md);
        return oom(e);
    }
    yyjson_mut_doc_set_root(md, params);
    if (!yyjson_mut_obj_add_strcpy(md, flt, "address", r->contract) ||
        !yyjson_mut_obj_add_strcpy(md, flt, "fromBlock", hf) ||
        !yyjson_mut_obj_add_strcpy(md, flt, "toBlock", ht) ||
        !yyjson_mut_obj_add_val(md, flt, "topics", topics) || !yyjson_mut_arr_add_val(params, flt)) {
        yyjson_mut_doc_free(md);
        return oom(e);
    }
    char *pj = mut_json(md);
    if (pj == NULL)
        return oom(e);
    yyjson_doc *d;
    yyjson_val *res;
    int dummy = 0;
    if (rpc_code == NULL)
        rpc_code = &dummy;
    av_status st = av_rpc_call(r, "eth_getLogs", pj, &d, &res, rpc_code, e);
    free(pj);
    if (st != AV_OK)
        return AV_ERR;
    st = av_events_parse(res, a, evs, n, e);
    yyjson_doc_free(d);
    return st;
}

av_status av_rpc_count(av_rpc *r, const char *query, uint64_t as_of, uint64_t *out_count, av_err *e)
{
    (void) as_of;
    if (r == NULL || out_count == NULL) {
        av_err_set(e, "invalid_argument", "null rpc or out_count pointer");
        return AV_ERR;
    }
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL)
        return oom(e);
    yyjson_mut_val *params = yyjson_mut_arr(md);
    yyjson_mut_doc_set_root(md, params);
    if (!yyjson_mut_arr_add_strcpy(md, params, query != NULL ? query : "")) {
        yyjson_mut_doc_free(md);
        return oom(e);
    }
    yyjson_mut_val *opts = yyjson_mut_obj(md);
    yyjson_mut_val *sel = yyjson_mut_obj(md);
    if (!opts || !sel || !yyjson_mut_obj_add_bool(md, sel, "count", true) ||
        !yyjson_mut_obj_add_val(md, opts, "select", sel) || !yyjson_mut_arr_add_val(params, opts)) {
        yyjson_mut_doc_free(md);
        return oom(e);
    }
    char *pj = mut_json(md);
    if (pj == NULL)
        return oom(e);

    yyjson_doc *d = NULL;
    yyjson_val *res = NULL;
    int rpc_code = 0;
    av_status st = av_rpc_call(r, "arkiv_query", pj, &d, &res, &rpc_code, e);
    free(pj);
    if (st != AV_OK)
        return AV_ERR;

    yyjson_val *cnt_val = yyjson_obj_get(res, "count");
    if (cnt_val == NULL) {
        yyjson_doc_free(d);
        av_err_set(e, "arkiv_rpc_error", "missing count field in arkiv_query response");
        return AV_ERR;
    }

    uint64_t count = 0;
    if (yyjson_is_str(cnt_val)) {
        const char *s = yyjson_get_str(cnt_val);
        if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
            count = strtoull(s + 2, NULL, 16);
        else
            count = strtoull(s, NULL, 10);
    } else if (yyjson_is_uint(cnt_val)) {
        count = yyjson_get_uint(cnt_val);
    }
    yyjson_doc_free(d);
    *out_count = count;
    return AV_OK;
}
