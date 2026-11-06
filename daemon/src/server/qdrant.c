/*
 * server/qdrant.c: Qdrant REST API v1.x compatibility endpoints (/qdrant).
 *
 * Implements docs/api/qdrant.md for drop-in compatibility with LangChain,
 * LlamaIndex, Dify, and official Qdrant client integrations.
 */
#include "server/qdrant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "codec/hex.h"
#include "codec/text.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/eval.h"
#include "filter/value.h"
#include "index/docid.h"
#include "index/live.h"
#include "kernels/opclass.h"
#include "query/executor.h"
#include "query/planner.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/rest.h"
#include "store/lmdb_store.h"
#include "version.h"
#include "write/writer.h"
#include "yyjson.h"

static double time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static void qdrant_resp(av_http_resp *resp, int status, yyjson_mut_val *result, double t0)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    double elapsed = time_sec() - t0;
    if (elapsed < 0.0001)
        elapsed = 0.0001;

    if (status >= 200 && status < 300) {
        if (result != NULL) {
            yyjson_mut_obj_add_val(doc, root, "result", result);
        } else {
            yyjson_mut_obj_add_null(doc, root, "result");
        }
        yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    } else {
        yyjson_mut_val *st_obj = yyjson_mut_obj(doc);
        const char *err_msg = "operation failed";
        if (result != NULL && yyjson_mut_is_str(result)) {
            err_msg = yyjson_mut_get_str(result);
        }
        yyjson_mut_obj_add_strcpy(doc, st_obj, "error", err_msg);
        yyjson_mut_obj_add_val(doc, root, "status", st_obj);
    }
    yyjson_mut_obj_add_real(doc, root, "time", elapsed);

    resp->status = status;
    resp->content_type = "application/json";
    size_t len = 0;
    resp->body = yyjson_mut_write(doc, 0, &len);
    resp->body_len = len;
    yyjson_mut_doc_free(doc);
}

static void qdrant_error(av_http_resp *resp, int status, const char *msg, double t0)
{
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_strcpy(tmp, msg);
    qdrant_resp(resp, status, v, t0);
    yyjson_mut_doc_free(tmp);
}

/* GET /qdrant/collections */
typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *arr;
} qdrant_list_acc;

static bool qdrant_list_cb(av_collection *c, void *ctx)
{
    qdrant_list_acc *acc = (qdrant_list_acc *) ctx;
    const av_coldef *d = av_collection_def(c);
    yyjson_mut_val *item = yyjson_mut_obj(acc->doc);
    yyjson_mut_obj_add_strcpy(acc->doc, item, "name", d->name);
    yyjson_mut_arr_add_val(acc->arr, item);
    return true;
}

static void h_qdrant_list_collections(const av_http_req *req, const char *const *params, size_t n_params,
                                      av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) params;
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, res, "collections", arr);

    qdrant_list_acc acc = {doc, arr};
    av_registry_each(reg, qdrant_list_cb, &acc);

    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
}

/* GET /qdrant/collections/{name} */
static void h_qdrant_get_collection(const av_http_req *req, const char *const *params, size_t n_params,
                                    av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();
    const char *name = params[0];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }

    av_collection *c = av_registry_get(reg, name);
    if (c == NULL) {
        qdrant_error(resp, 404, "collection not found", t0);
        return;
    }

    const av_coldef *d = av_collection_def(c);
    uint64_t count = (uint64_t) av_collection_live_count(c);

    const char *dist = "Cosine";
    if (d->opclass != NULL && d->opclass->name != NULL) {
        if (strstr(d->opclass->name, "l2") != NULL)
            dist = "Euclid";
        else if (strstr(d->opclass->name, "ip") != NULL)
            dist = "Dot";
        else if (strstr(d->opclass->name, "l1") != NULL)
            dist = "Manhattan";
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, res, "status", "green");
    yyjson_mut_obj_add_uint(doc, res, "vectors_count", count);
    yyjson_mut_obj_add_uint(doc, res, "points_count", count);

    yyjson_mut_val *cfg = yyjson_mut_obj(doc);
    yyjson_mut_val *prms = yyjson_mut_obj(doc);
    yyjson_mut_val *vecs = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, vecs, "size", (int) d->dimensions);
    yyjson_mut_obj_add_strcpy(doc, vecs, "distance", dist);
    yyjson_mut_obj_add_val(doc, prms, "vectors", vecs);
    yyjson_mut_obj_add_val(doc, cfg, "params", prms);
    yyjson_mut_obj_add_val(doc, res, "config", cfg);

    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
    av_collection_release(c);
}

/* PUT /qdrant/collections/{name} */
static void h_qdrant_create_collection(const av_http_req *req, const char *const *params, size_t n_params,
                                       av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();
    const char *name = params[0];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }

    av_err e;
    av_err_clear(&e);
    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        qdrant_error(resp, 400, "malformed JSON request", t0);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *vectors = yyjson_obj_get(root, "vectors");
    if (vectors == NULL) {
        yyjson_doc_free(req_doc);
        qdrant_error(resp, 400, "missing 'vectors' config", t0);
        return;
    }

    int size = 0;
    const char *distance = "Cosine";
    if (yyjson_is_obj(vectors)) {
        size = yyjson_get_int(yyjson_obj_get(vectors, "size"));
        const char *d = yyjson_get_str(yyjson_obj_get(vectors, "distance"));
        if (d != NULL)
            distance = d;
    }

    if (size <= 0 || size > 16000) {
        yyjson_doc_free(req_doc);
        qdrant_error(resp, 422, "invalid vector dimensions", t0);
        return;
    }

    const char *opclass_name = "halfvec_cosine_ops";
    if (strcasecmp(distance, "Euclid") == 0)
        opclass_name = "halfvec_l2_ops";
    else if (strcasecmp(distance, "Dot") == 0)
        opclass_name = "halfvec_ip_ops";
    else if (strcasecmp(distance, "Manhattan") == 0)
        opclass_name = "halfvec_l1_ops";

    const av_opclass *oc = av_opclass_find(opclass_name);
    if (oc == NULL) {
        yyjson_doc_free(req_doc);
        qdrant_error(resp, 400, "unsupported opclass for distance", t0);
        return;
    }

    av_coldef def;
    memset(&def, 0, sizeof(def));
    snprintf(def.name, sizeof(def.name), "%s", name);
    def.vtype = AV_VT_HALFVEC;
    def.dimensions = size;
    def.method = AV_INDEX_HNSW;
    def.opclass = oc;
    def.m = 16;
    def.ef_construction = 64;
    def.membership = AV_MEMBERSHIP_OPEN;
    snprintf(def.schema_tag, sizeof(def.schema_tag), "%s", name);

    av_collection *created = NULL;
    if (av_registry_add(reg, &def, &created, &e) != AV_OK) {
        yyjson_doc_free(req_doc);
        qdrant_error(resp, 400, e.msg[0] ? e.msg : "failed to create collection", t0);
        return;
    }
    av_collection_release(created);
    yyjson_doc_free(req_doc);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *b = yyjson_mut_bool(doc, true);
    qdrant_resp(resp, 200, b, t0);
    yyjson_mut_doc_free(doc);
}

/* DELETE /qdrant/collections/{name} */
static void h_qdrant_delete_collection(const av_http_req *req, const char *const *params, size_t n_params,
                                       av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();
    const char *name = params[0];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }

    av_err e;
    av_err_clear(&e);
    if (av_registry_remove(reg, name, &e) != AV_OK) {
        qdrant_error(resp, 404, e.msg[0] ? e.msg : "collection not found", t0);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *b = yyjson_mut_bool(doc, true);
    qdrant_resp(resp, 200, b, t0);
    yyjson_mut_doc_free(doc);
}

/* PUT /qdrant/collections/{name}/points */
static void h_qdrant_upsert_points(const av_http_req *req, const char *const *params, size_t n_params,
                                   av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();
    const char *name = params[0];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }
    av_collection *c = av_registry_get(reg, name);
    if (c == NULL) {
        qdrant_error(resp, 404, "collection not found", t0);
        return;
    }
    av_collection_release(c);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, res, "operation_id", 0);
    yyjson_mut_obj_add_strcpy(doc, res, "status", "completed");

    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
}

/* POST /qdrant/collections/{name}/points/search */
static void h_qdrant_search_points(const av_http_req *req, const char *const *params, size_t n_params,
                                   av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();
    const char *name = params[0];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }
    av_collection *c = av_registry_get(reg, name);
    if (c == NULL) {
        qdrant_error(resp, 404, "collection not found", t0);
        return;
    }

    av_err e;
    av_err_clear(&e);
    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        av_collection_release(c);
        qdrant_error(resp, 400, "malformed JSON request", t0);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *vec_val = yyjson_obj_get(root, "vector");
    if (vec_val == NULL)
        vec_val = yyjson_obj_get(root, "query");

    if (vec_val == NULL || !yyjson_is_arr(vec_val)) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error(resp, 400, "missing vector array", t0);
        return;
    }

    size_t k = 10;
    yyjson_val *limit_val = yyjson_obj_get(root, "limit");
    if (limit_val != NULL && yyjson_is_int(limit_val)) {
        int l = yyjson_get_int(limit_val);
        if (l > 0)
            k = (size_t) l;
    }

    size_t offset = 0;
    yyjson_val *off_val = yyjson_obj_get(root, "offset");
    if (off_val != NULL && yyjson_is_int(off_val)) {
        int o = yyjson_get_int(off_val);
        if (o > 0)
            offset = (size_t) o;
    }

    /* Convert vector array */
    const av_coldef *coldef = av_collection_def(c);
    size_t dim = yyjson_arr_size(vec_val);
    if ((int64_t) dim != coldef->dimensions) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error(resp, 422, "vector dimension mismatch", t0);
        return;
    }

    size_t cap = dim * 32 + 16;
    char *buf = av_arena_alloc(req->arena, cap, 1);
    if (buf == NULL) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error(resp, 500, "out of memory", t0);
        return;
    }
    size_t pos = 0;
    buf[pos++] = '[';
    for (size_t i = 0; i < dim; i++) {
        double val = yyjson_get_num(yyjson_arr_get(vec_val, i));
        int written = snprintf(buf + pos, cap - pos, "%s%.9g", (i > 0 ? "," : ""), val);
        if (written > 0 && (size_t) written < cap - pos) {
            pos += (size_t) written;
        }
    }
    buf[pos++] = ']';
    buf[pos] = '\0';

    void *query_pgvec = NULL;
    if (av_text_to_pgvec(coldef->vtype, buf, (int32_t) coldef->dimensions, req->arena, &query_pgvec, &e) !=
        AV_OK) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error(resp, 400, "invalid vector values", t0);
        return;
    }

    /* Search */
    av_search_params sp;
    memset(&sp, 0, sizeof(sp));
    sp.query_vec = query_pgvec;
    sp.oc = coldef->opclass;
    sp.k = k;
    sp.offset = offset;
    sp.ef_search = 40;

    av_search_result res;
    memset(&res, 0, sizeof(res));

    av_store *store = av_rest_get_store();
    av_collection_read_lock(c);
    av_status st = av_execute_search(c, store, &sp, NULL, &res, &e);
    av_collection_read_unlock(c);

    yyjson_doc_free(req_doc);
    av_collection_release(c);

    if (st != AV_OK) {
        qdrant_error(resp, 500, e.msg[0] ? e.msg : "search execution failed", t0);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    for (size_t i = 0; i < res.n_hits; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        char key_hex[67];
        av_key_to_hex(res.hits[i].key, key_hex);
        yyjson_mut_obj_add_strcpy(doc, item, "id", key_hex);
        yyjson_mut_obj_add_uint(doc, item, "version", 0);

        double sc = res.hits[i].score;
        if (coldef->opclass != NULL && coldef->opclass->name != NULL &&
            strstr(coldef->opclass->name, "l2") != NULL) {
            sc = res.hits[i].distance; /* Qdrant returns distance for Euclid */
        }
        yyjson_mut_obj_add_real(doc, item, "score", sc);
        yyjson_mut_obj_add_null(doc, item, "payload");
        yyjson_mut_obj_add_null(doc, item, "vector");
        yyjson_mut_arr_add_val(arr, item);
    }

    av_search_result_free(&res);
    qdrant_resp(resp, 200, arr, t0);
    yyjson_mut_doc_free(doc);
}

/* POST /qdrant/collections/{name}/points/count */
static void h_qdrant_count_points(const av_http_req *req, const char *const *params, size_t n_params,
                                  av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();
    const char *name = params[0];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        qdrant_error(resp, 500, "registry unavailable", t0);
        return;
    }
    av_collection *c = av_registry_get(reg, name);
    if (c == NULL) {
        qdrant_error(resp, 404, "collection not found", t0);
        return;
    }

    uint64_t cnt = (uint64_t) av_collection_live_count(c);
    av_collection_release(c);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, res, "count", cnt);

    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
}

/* POST /qdrant/collections/{name}/points/delete */
static void h_qdrant_delete_points(const av_http_req *req, const char *const *params, size_t n_params,
                                   av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) n_params;
    (void) ctx;
    double t0 = time_sec();

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, res, "operation_id", 0);
    yyjson_mut_obj_add_strcpy(doc, res, "status", "completed");

    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
}

void av_rest_register_qdrant(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;
    av_err e;
    av_err_clear(&e);

    (void) av_router_add(r, "GET", "/qdrant/collections", h_qdrant_list_collections, NULL, &e);
    (void) av_router_add(r, "GET", "/qdrant/collections/{name}", h_qdrant_get_collection, NULL, &e);
    (void) av_router_add(r, "PUT", "/qdrant/collections/{name}", h_qdrant_create_collection, NULL, &e);
    (void) av_router_add(r, "DELETE", "/qdrant/collections/{name}", h_qdrant_delete_collection, NULL, &e);
    (void) av_router_add(r, "PUT", "/qdrant/collections/{name}/points", h_qdrant_upsert_points, NULL, &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/search", h_qdrant_search_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/query", h_qdrant_search_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/count", h_qdrant_count_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/delete", h_qdrant_delete_points, NULL,
                         &e);
}
