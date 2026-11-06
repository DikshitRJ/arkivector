/*
 * server/rest_ops.c: operational endpoints (/healthz, /readyz, /v1/version, /v1/sync, /metrics)
 * and the registration hooks declared in rest.h.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "kernels/dispatch.h"
#include "server/json_io.h"
#include "server/metrics.h"
#include "server/problem.h"
#include "server/rest.h"
#include "version.h"
#include "yyjson.h"

#define MAX_REGISTRARS   16
#define MAX_CAPABILITIES 128

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static struct {
    av_rest_registrar fn;
    void *ctx;
} g_registrars[MAX_REGISTRARS];
static size_t g_n_registrars;

static int g_ready_explicit = -1; /* -1 auto, 0 false, 1 true */
static av_sync_provider_fn g_sync_fn;
static void *g_sync_ctx;
static int64_t g_chain_id;
static bool g_writes_enabled;
static char *g_caps[MAX_CAPABILITIES];
static size_t g_n_caps;

static struct av_registry *g_reg_svc = NULL;
static struct av_store *g_store_svc = NULL;
static struct av_rpc *g_rpc_svc = NULL;
static struct av_sync *g_sync_svc = NULL;
static struct av_writer *g_writer_svc = NULL;
static struct av_idempotency_store *g_idem_svc = NULL;

void av_rest_set_services(struct av_registry *reg, struct av_store *store, struct av_rpc *rpc, struct av_sync *sync)
{
    pthread_mutex_lock(&g_mu);
    g_reg_svc = reg;
    g_store_svc = store;
    g_rpc_svc = rpc;
    g_sync_svc = sync;
    pthread_mutex_unlock(&g_mu);
}

void av_rest_set_writer(struct av_writer *writer, struct av_idempotency_store *idem)
{
    pthread_mutex_lock(&g_mu);
    g_writer_svc = writer;
    g_idem_svc = idem;
    pthread_mutex_unlock(&g_mu);
}

struct av_registry *av_rest_get_registry(void)
{
    pthread_mutex_lock(&g_mu);
    struct av_registry *r = g_reg_svc;
    pthread_mutex_unlock(&g_mu);
    return r;
}

struct av_store *av_rest_get_store(void)
{
    pthread_mutex_lock(&g_mu);
    struct av_store *s = g_store_svc;
    pthread_mutex_unlock(&g_mu);
    return s;
}

struct av_rpc *av_rest_get_rpc(void)
{
    pthread_mutex_lock(&g_mu);
    struct av_rpc *r = g_rpc_svc;
    pthread_mutex_unlock(&g_mu);
    return r;
}

struct av_sync *av_rest_get_sync(void)
{
    pthread_mutex_lock(&g_mu);
    struct av_sync *s = g_sync_svc;
    pthread_mutex_unlock(&g_mu);
    return s;
}

struct av_writer *av_rest_get_writer(void)
{
    pthread_mutex_lock(&g_mu);
    struct av_writer *w = g_writer_svc;
    pthread_mutex_unlock(&g_mu);
    return w;
}

struct av_idempotency_store *av_rest_get_idempotency(void)
{
    pthread_mutex_lock(&g_mu);
    struct av_idempotency_store *i = g_idem_svc;
    pthread_mutex_unlock(&g_mu);
    return i;
}

av_status av_rest_add_registrar(av_rest_registrar fn, void *ctx, av_err *e)
{
    av_status st = AV_OK;
    pthread_mutex_lock(&g_mu);
    if (g_n_registrars == MAX_REGISTRARS) {
        av_err_set(e, "internal", "too many REST registrars");
        st = AV_ERR;
    } else {
        g_registrars[g_n_registrars].fn = fn;
        g_registrars[g_n_registrars].ctx = ctx;
        g_n_registrars++;
    }
    pthread_mutex_unlock(&g_mu);
    return st;
}

void av_rest_set_ready(bool ready)
{
    pthread_mutex_lock(&g_mu);
    g_ready_explicit = ready ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
}

void av_rest_set_sync_provider(av_sync_provider_fn fn, void *ctx)
{
    pthread_mutex_lock(&g_mu);
    g_sync_fn = fn;
    g_sync_ctx = ctx;
    pthread_mutex_unlock(&g_mu);
}

void av_rest_set_info(int64_t chain_id, bool writes_enabled)
{
    pthread_mutex_lock(&g_mu);
    g_chain_id = chain_id;
    g_writes_enabled = writes_enabled;
    pthread_mutex_unlock(&g_mu);
}

av_status av_rest_add_capability(const char *capability, av_err *e)
{
    av_status st = AV_OK;
    pthread_mutex_lock(&g_mu);
    bool have = false;
    for (size_t i = 0; i < g_n_caps; i++) {
        have = have || strcmp(g_caps[i], capability) == 0;
    }
    if (!have) {
        size_t n = strlen(capability) + 1;
        char *copy = g_n_caps < MAX_CAPABILITIES ? malloc(n) : NULL;
        if (copy == NULL) {
            av_err_set(e, "internal", "cannot add capability '%s'", capability);
            st = AV_ERR;
        } else {
            memcpy(copy, capability, n);
            g_caps[g_n_caps++] = copy;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return st;
}

const char *const *av_rest_capabilities(size_t *n)
{
    pthread_mutex_lock(&g_mu);
    const char **out = calloc(g_n_caps + 1, sizeof(char *));
    *n = 0;
    if (out != NULL) {
        for (size_t i = 0; i < g_n_caps; i++) {
            out[i] = g_caps[i];
        }
        *n = g_n_caps;
    }
    pthread_mutex_unlock(&g_mu);
    return out;
}

/* ---- handlers ---- */

static void fail(av_http_resp *resp, const char *msg)
{
    av_err e;
    av_err_set(&e, "internal", "%s", msg);
    av_problem_from_err(&e, resp, NULL);
}

static void h_healthz(const av_http_req *req, const char *const *params, size_t n, av_http_resp *resp,
                      void *ctx)
{
    (void) req;
    (void) params;
    (void) n;
    (void) ctx;
    av_resp_json_str(resp, 200, "{\"status\":\"ok\"}");
}

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *obj;
    bool all_live;
    bool oom;
} ready_acc;

static void ready_visit(const av_sync_status *s, void *vctx)
{
    ready_acc *a = (ready_acc *) vctx;
    if (!yyjson_mut_obj_add_strcpy(a->doc, a->obj, s->name, s->state)) {
        a->oom = true;
    }
    if (strcmp(s->state, "live") != 0) {
        a->all_live = false;
    }
}

static void h_readyz(const av_http_req *req, const char *const *params, size_t n, av_http_resp *resp,
                     void *ctx)
{
    (void) req;
    (void) params;
    (void) n;
    (void) ctx;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        fail(resp, "out of memory");
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    ready_acc acc = {doc, yyjson_mut_obj(doc), true, false};
    pthread_mutex_lock(&g_mu);
    int explicit_ready = g_ready_explicit;
    av_sync_provider_fn fn = g_sync_fn;
    void *pctx = g_sync_ctx;
    pthread_mutex_unlock(&g_mu);
    if (fn != NULL) {
        fn(ready_visit, &acc, pctx);
    }
    bool ready = explicit_ready >= 0 ? explicit_ready == 1 : acc.all_live;
    yyjson_mut_obj_add_bool(doc, root, "ready", ready);
    yyjson_mut_obj_add_val(doc, root, "collections", acc.obj);
    if (acc.oom || av_resp_json(resp, ready ? 200 : 503, doc) != AV_OK) {
        if (acc.oom) {
            yyjson_mut_doc_free(doc);
        }
        fail(resp, "cannot render readiness");
        return;
    }
    if (!ready) {
        resp->retry_after = "1";
    }
}

static void h_version(const av_http_req *req, const char *const *params, size_t n, av_http_resp *resp,
                      void *ctx)
{
    (void) req;
    (void) params;
    (void) n;
    (void) ctx;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        fail(resp, "out of memory");
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    const char *pgv = AV_PGVECTOR_TAG;
    if (pgv[0] == 'v') {
        pgv++;
    }
    yyjson_mut_obj_add_str(doc, root, "daemon_version", AV_DAEMON_VERSION);
    yyjson_mut_obj_add_str(doc, root, "api_version", AV_API_VERSION);
    yyjson_mut_obj_add_str(doc, root, "pgvector_version", pgv);
    yyjson_mut_obj_add_strcpy(doc, root, "kernel_tier", av_kernel_tier());
    pthread_mutex_lock(&g_mu);
    yyjson_mut_obj_add_sint(doc, root, "chain_id", g_chain_id);
    yyjson_mut_obj_add_bool(doc, root, "writes_enabled", g_writes_enabled);
    yyjson_mut_val *caps = yyjson_mut_arr(doc);
    for (size_t i = 0; i < g_n_caps; i++) {
        yyjson_mut_arr_add_strcpy(doc, caps, g_caps[i]);
    }
    pthread_mutex_unlock(&g_mu);
    yyjson_mut_obj_add_val(doc, root, "capabilities", caps);
    if (av_resp_json(resp, 200, doc) != AV_OK) {
        fail(resp, "cannot render version");
    }
}

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *arr;
} sync_acc;

static void sync_visit(const av_sync_status *s, void *vctx)
{
    sync_acc *a = (sync_acc *) vctx;
    yyjson_mut_val *o = yyjson_mut_obj(a->doc);
    yyjson_mut_obj_add_strcpy(a->doc, o, "name", s->name);
    yyjson_mut_obj_add_strcpy(a->doc, o, "state", s->state);
    yyjson_mut_obj_add_uint(a->doc, o, "as_of_block", s->as_of_block);
    yyjson_mut_obj_add_uint(a->doc, o, "head_block", s->head_block);
    yyjson_mut_obj_add_uint(a->doc, o, "lag_blocks",
                            s->head_block > s->as_of_block ? s->head_block - s->as_of_block : 0);
    if (s->last_error != NULL) {
        yyjson_mut_obj_add_strcpy(a->doc, o, "last_error", s->last_error);
    } else {
        yyjson_mut_obj_add_null(a->doc, o, "last_error");
    }
    yyjson_mut_arr_append(a->arr, o);
}

static void h_sync(const av_http_req *req, const char *const *params, size_t n, av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) params;
    (void) n;
    (void) ctx;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        fail(resp, "out of memory");
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    sync_acc acc = {doc, yyjson_mut_arr(doc)};
    pthread_mutex_lock(&g_mu);
    av_sync_provider_fn fn = g_sync_fn;
    void *pctx = g_sync_ctx;
    pthread_mutex_unlock(&g_mu);
    if (fn != NULL) {
        fn(sync_visit, &acc, pctx);
    }
    yyjson_mut_obj_add_val(doc, root, "collections", acc.arr);
    if (av_resp_json(resp, 200, doc) != AV_OK) {
        fail(resp, "cannot render sync status");
    }
}

static void h_metrics(const av_http_req *req, const char *const *params, size_t n, av_http_resp *resp,
                      void *ctx)
{
    (void) req;
    (void) params;
    (void) n;
    (void) ctx;
    size_t len = 0;
    char *text = av_metrics_render(&len);
    if (text == NULL) {
        fail(resp, "cannot render metrics");
        return;
    }
    free(resp->body);
    resp->body = text;
    resp->body_len = len;
    resp->status = 200;
    resp->content_type = "text/plain; version=0.0.4; charset=utf-8";
}

av_status av_rest_register_all(av_router *r, bool debug_routes, av_err *e)
{
    static const char *const initial_caps[] = {
        "ops.version",
        "ops.sync",
        "ops.metrics",
        "collections.read",
        "collections.write",
        "collections.follow",
        "entities.read",
        "search.exact",
        "search.hnsw",
        "search.filter.json",
        "search.filter.where",
        "search.batch",
        "search.explain",
        "scroll",
        "count",
        "consistency.min_block",
        "collections.manifest",
        "entities.write",
        "entities.prepare",
        "entities.verify",
        "idempotency",
        "auth.api_keys",
        "qdrant",
        "mcp"
    };
    for (size_t i = 0; i < sizeof(initial_caps) / sizeof(initial_caps[0]); i++) {
        if (av_rest_add_capability(initial_caps[i], e) != AV_OK) {
            return AV_ERR;
        }
    }
    if (av_router_add(r, "GET", "/healthz", h_healthz, NULL, e) != AV_OK ||
        av_router_add(r, "GET", "/readyz", h_readyz, NULL, e) != AV_OK ||
        av_router_add(r, "GET", "/v1/version", h_version, NULL, e) != AV_OK ||
        av_router_add(r, "GET", "/v1/sync", h_sync, NULL, e) != AV_OK ||
        av_router_add(r, "GET", "/metrics", h_metrics, NULL, e) != AV_OK) {
        return AV_ERR;
    }
    av_rest_register_collections(r, debug_routes, NULL);
    av_rest_register_entities(r, debug_routes, NULL);
    av_rest_register_search(r, debug_routes, NULL);

    pthread_mutex_lock(&g_mu);
    size_t n = g_n_registrars;
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < n; i++) {
        g_registrars[i].fn(r, debug_routes, g_registrars[i].ctx);
    }
    return AV_OK;
}
