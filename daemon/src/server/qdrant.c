/*
 * server/qdrant.c: Qdrant REST API v1.x compatibility endpoints (/qdrant).
 *
 * Implements docs/api/qdrant.md for drop-in compatibility with LangChain,
 * LlamaIndex, Dify, and official Qdrant client integrations.
 */
#include "server/qdrant.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "codec/base64.h"
#include "codec/hex.h"
#include "codec/text.h"
#include "codec/vectorbytes.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/eval.h"
#include "filter/json_filter.h"
#include "filter/value.h"
#include "index/docid.h"
#include "index/live.h"
#include "kernels/opclass.h"
#include "query/executor.h"
#include "query/planner.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/qdrant_map.h"
#include "server/rest.h"
#include "store/lmdb_store.h"
#include "version.h"
#include "write/writer.h"
#include "yyjson.h"

/* Qdrant REST version whose shapes /qdrant follows (reported by GET /qdrant for client checks). */
#define AV_QDRANT_COMPAT_VERSION "1.12.0"
/* default_ttl of collections created with PUT /qdrant/collections/{name} */
#define AV_QDRANT_DEFAULT_TTL_DAYS 30

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
    /* Qdrant points have no lifetime: creates through /qdrant get 30 days (extend with the REST API). */
    def.default_ttl.kind = AV_TTL_DAYS;
    def.default_ttl.value = AV_QDRANT_DEFAULT_TTL_DAYS;
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

/* Qdrant-shaped error from an av_err: HTTP status from the error registry. */
static void qdrant_error_err(av_http_resp *resp, const av_err *e, double t0)
{
    int status = e->code[0] != '\0' ? av_problem_status(e->code) : 500;
    char msg[600];
    snprintf(msg, sizeof(msg), "%s: %s", e->code[0] != '\0' ? e->code : "internal",
             e->msg[0] != '\0' ? e->msg : "operation failed");
    qdrant_error(resp, status, msg, t0);
}

static _Atomic uint64_t g_operation_id = 1;

static void qdrant_update_result(av_http_resp *resp, bool completed, double t0)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, res, "operation_id", atomic_fetch_add(&g_operation_id, 1));
    yyjson_mut_obj_add_strcpy(doc, res, "status", completed ? "completed" : "acknowledged");
    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
}

/* ?wait=true (Qdrant's default is false). */
static bool qdrant_wait(const av_http_req *req)
{
    if (req->query == NULL)
        return false;
    const char *p = strstr(req->query, "wait=");
    if (p == NULL || (p != req->query && p[-1] != '&'))
        return false;
    return strncmp(p + 5, "true", 4) == 0 && (p[9] == '\0' || p[9] == '&');
}

/* The writer, or a Qdrant-shaped 403 when the daemon has no signer (never a fake success). */
static av_writer *qdrant_writer(av_http_resp *resp, double t0)
{
    av_writer *w = av_rest_get_writer();
    if (w == NULL || av_writer_get_signer(w) == NULL) {
        qdrant_error(resp, 403,
                     "write_disabled: this daemon has no signer (set ARKIV_PRIVATE_KEY); the Qdrant API is "
                     "read-only",
                     t0);
        return NULL;
    }
    return w;
}

/* PUT /qdrant/collections/{name}/points */
static void h_qdrant_upsert_points(const av_http_req *req, const char *const *params, size_t n_params,
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
    av_writer *w = qdrant_writer(resp, t0);
    if (w == NULL) {
        av_collection_release(c);
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
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *ents = NULL;
    size_t dropped = 0;
    yyjson_doc *edoc = NULL;
    av_status st = av_qdrant_points_to_entities(yyjson_doc_get_root(req_doc), mdoc, &ents, &dropped, &e);
    if (st == AV_OK) {
        edoc = yyjson_mut_val_imut_copy(ents, NULL);
        if (edoc == NULL) {
            av_err_set(&e, "internal", "out of memory");
            st = AV_ERR;
        }
    }
    yyjson_mut_doc_free(mdoc);
    yyjson_doc_free(req_doc);
    if (st != AV_OK) {
        av_collection_release(c);
        qdrant_error_err(resp, &e, t0);
        return;
    }

    bool wait = qdrant_wait(req);
    av_upsert_batch_result out;
    memset(&out, 0, sizeof(out));
    st = av_writer_upsert(w, c, yyjson_doc_get_root(edoc), wait ? AV_WAIT_INDEXED : AV_WAIT_NONE, 60000,
                          req->arena, &out, &e);
    yyjson_doc_free(edoc);
    av_collection_release(c);
    if (st != AV_OK) {
        qdrant_error_err(resp, &e, t0);
        return;
    }
    if (dropped > 0) {
        char *wmsg = av_arena_alloc(req->arena, 160, 1);
        if (wmsg != NULL) {
            snprintf(wmsg, 160,
                     "199 arkivector \"%zu payload field(s) dropped: nested values are kept only without a "
                     "page_content/text key\"",
                     dropped);
            resp->warning = wmsg;
        }
    }
    qdrant_update_result(resp, wait, t0);
}

/* Qdrant filter -> compiled filter AST; *ast stays NULL for no filter. */
static av_status qdrant_compile_filter(yyjson_val *qf, const av_coldef *def, av_arena *arena, av_filter **ast,
                                       av_err *e)
{
    *ast = NULL;
    if (qf == NULL || yyjson_is_null(qf))
        return AV_OK;
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *f = NULL;
    av_status st = av_qdrant_filter_to_json(qf, md, &f, e);
    if (st == AV_OK && f != NULL) {
        yyjson_doc *fd = yyjson_mut_val_imut_copy(f, NULL);
        if (fd == NULL) {
            av_err_set(e, "internal", "out of memory");
            st = AV_ERR;
        } else {
            st = av_filter_from_json(yyjson_doc_get_root(fd), def, arena, ast, e);
            yyjson_doc_free(fd);
        }
    }
    yyjson_mut_doc_free(md);
    return st;
}

/* Vector of a hit (base64 VectorBytes) as a JSON number array, or null. */
static yyjson_mut_val *hit_vector(yyjson_mut_doc *doc, const char *b64, av_vtype vt, av_arena *arena)
{
    av_err e;
    av_err_clear(&e);
    uint8_t *raw = NULL;
    size_t rl = 0;
    void *pg = NULL;
    char *txt = NULL;
    if (b64 == NULL || av_base64_decode(b64, strlen(b64), arena, &raw, &rl, &e) != AV_OK ||
        av_vb_decode(raw, rl, arena, &pg, NULL, &e) != AV_OK ||
        av_pgvec_to_text(vt, pg, arena, &txt, &e) != AV_OK)
        return yyjson_mut_null(doc);
    if (vt != AV_VT_VECTOR && vt != AV_VT_HALFVEC)
        return yyjson_mut_strcpy(doc, txt);
    yyjson_doc *jd = yyjson_read(txt, strlen(txt), 0);
    if (jd == NULL)
        return yyjson_mut_null(doc);
    yyjson_mut_val *v = yyjson_val_mut_copy(doc, yyjson_doc_get_root(jd));
    yyjson_doc_free(jd);
    return v;
}

/* POST /qdrant/collections/{name}/points/search and .../points/query */
static void qdrant_search(const av_http_req *req, const char *const *params, av_http_resp *resp,
                          bool query_api)
{
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
    if (yyjson_is_obj(vec_val) && yyjson_obj_get(vec_val, "nearest") != NULL)
        vec_val = yyjson_obj_get(vec_val, "nearest");

    if (vec_val == NULL || !yyjson_is_arr(vec_val)) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error(resp, vec_val == NULL ? 400 : 501,
                     vec_val == NULL ? "missing vector array"
                                     : "only nearest-vector queries are supported (no named vectors or ids)",
                     t0);
        return;
    }

    size_t k = 10;
    yyjson_val *limit_val = yyjson_obj_get(root, "limit");
    if (limit_val == NULL)
        limit_val = yyjson_obj_get(root, "top");
    if (limit_val != NULL && yyjson_is_int(limit_val)) {
        int64_t l = yyjson_get_sint(limit_val);
        if (l > 0)
            k = (size_t) (l > 10000 ? 10000 : l);
    }

    size_t offset = 0;
    yyjson_val *off_val = yyjson_obj_get(root, "offset");
    if (off_val != NULL && yyjson_is_int(off_val)) {
        int64_t o = yyjson_get_sint(off_val);
        if (o > 0)
            offset = (size_t) (o > 100000 ? 100000 : o);
    }

    const av_coldef *coldef = av_collection_def(c);
    bool euclid = coldef->opclass != NULL && coldef->opclass->name != NULL &&
                  strstr(coldef->opclass->name, "l2") != NULL;

    /* Convert vector array */
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

    av_filter *ast = NULL;
    if (qdrant_compile_filter(yyjson_obj_get(root, "filter"), coldef, req->arena, &ast, &e) != AV_OK) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error_err(resp, &e, t0);
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
    yyjson_val *thr = yyjson_obj_get(root, "score_threshold");
    if (thr != NULL && yyjson_is_num(thr)) {
        if (euclid) {
            sp.has_max_distance = true; /* Qdrant's Euclid score is the distance */
            sp.max_distance = yyjson_get_num(thr);
        } else {
            sp.has_min_score = true;
            sp.min_score = yyjson_get_num(thr);
        }
    }
    yyjson_val *wp = yyjson_obj_get(root, "with_payload");
    yyjson_val *wv = yyjson_obj_get(root, "with_vector");
    if (wv == NULL)
        wv = yyjson_obj_get(root, "with_vectors");
    bool want_payload = av_qdrant_wants_payload(wp);
    bool want_vector = yyjson_is_bool(wv) && yyjson_get_bool(wv);
    if (want_payload)
        sp.include_flags |= AV_INCLUDE_ATTRIBUTES | AV_INCLUDE_DOCUMENT;
    if (want_vector)
        sp.include_flags |= AV_INCLUDE_VECTOR;

    av_search_result res;
    memset(&res, 0, sizeof(res));

    av_store *store = av_rest_get_store();
    av_collection_read_lock(c);
    av_status st = AV_OK;
    roaring_bitmap_t *cand = NULL;
    if (ast != NULL) {
        cand = av_filter_eval(ast, av_collection_attr_index(c), av_collection_docmap(c),
                              av_collection_live(c), av_collection_as_of(c), &e);
        if (cand == NULL && ast->kind != AV_F_TRUE)
            st = AV_ERR;
    }
    if (st == AV_OK)
        st = av_execute_search(c, store, &sp, cand, &res, &e);
    av_collection_read_unlock(c);
    if (cand != NULL)
        roaring_bitmap_free(cand);

    if (st != AV_OK) {
        yyjson_doc_free(req_doc);
        av_collection_release(c);
        qdrant_error_err(resp, &e, t0);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    for (size_t i = 0; i < res.n_hits; i++) {
        const av_search_hit *h = &res.hits[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        char key_hex[67];
        av_key_to_hex(h->key, key_hex);
        yyjson_mut_obj_add_val(doc, item, "id", av_qdrant_id_json(doc, h->has_id ? h->id : key_hex));
        yyjson_mut_obj_add_uint(doc, item, "version", h->updated_at);
        yyjson_mut_obj_add_real(doc, item, "score", euclid ? h->distance : h->score);
        if (want_payload) {
            /* attributes_obj belongs to res.doc: copy it into this doc */
            yyjson_mut_val *attrs =
                h->attributes_obj != NULL ? yyjson_mut_val_mut_copy(doc, h->attributes_obj) : NULL;
            yyjson_mut_obj_add_val(doc, item, "payload",
                                   av_qdrant_payload(doc, attrs, h->document, h->doc_len, wp));
        } else {
            yyjson_mut_obj_add_null(doc, item, "payload");
        }
        if (want_vector)
            yyjson_mut_obj_add_val(doc, item, "vector",
                                   hit_vector(doc, h->vector_bytes, coldef->vtype, req->arena));
        else
            yyjson_mut_obj_add_null(doc, item, "vector");
        yyjson_mut_arr_add_val(arr, item);
    }

    av_search_result_free(&res);
    yyjson_doc_free(req_doc);
    av_collection_release(c);
    if (query_api) {
        yyjson_mut_val *qr = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, qr, "points", arr);
        qdrant_resp(resp, 200, qr, t0);
    } else {
        qdrant_resp(resp, 200, arr, t0);
    }
    yyjson_mut_doc_free(doc);
}

static void h_qdrant_search_points(const av_http_req *req, const char *const *params, size_t n_params,
                                   av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    qdrant_search(req, params, resp, false);
}

static void h_qdrant_query_points(const av_http_req *req, const char *const *params, size_t n_params,
                                  av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    qdrant_search(req, params, resp, true);
}

/* POST /qdrant/collections/{name}/points/count */
static void h_qdrant_count_points(const av_http_req *req, const char *const *params, size_t n_params,
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
    av_filter *ast = NULL;
    if (req->body_len > 0) {
        yyjson_doc *req_doc = NULL;
        if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
            av_collection_release(c);
            qdrant_error(resp, 400, "malformed JSON request", t0);
            return;
        }
        av_status st = qdrant_compile_filter(yyjson_obj_get(yyjson_doc_get_root(req_doc), "filter"),
                                             av_collection_def(c), req->arena, &ast, &e);
        yyjson_doc_free(req_doc);
        if (st != AV_OK) {
            av_collection_release(c);
            qdrant_error_err(resp, &e, t0);
            return;
        }
    }

    uint64_t cnt = 0;
    if (ast == NULL) {
        cnt = (uint64_t) av_collection_live_count(c);
    } else {
        av_collection_read_lock(c);
        roaring_bitmap_t *cand = av_filter_eval(ast, av_collection_attr_index(c), av_collection_docmap(c),
                                                av_collection_live(c), av_collection_as_of(c), &e);
        av_collection_read_unlock(c);
        if (cand == NULL && ast->kind != AV_F_TRUE) {
            av_collection_release(c);
            qdrant_error_err(resp, &e, t0);
            return;
        }
        cnt = cand != NULL ? roaring_bitmap_get_cardinality(cand) : (uint64_t) av_collection_live_count(c);
        if (cand != NULL)
            roaring_bitmap_free(cand);
    }
    av_collection_release(c);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, res, "count", cnt);

    qdrant_resp(resp, 200, res, t0);
    yyjson_mut_doc_free(doc);
}

/* POST /qdrant/collections/{name}/points/delete: {"points": [ids]} or {"filter": {...}} */
static void h_qdrant_delete_points(const av_http_req *req, const char *const *params, size_t n_params,
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
    av_writer *w = qdrant_writer(resp, t0);
    if (w == NULL) {
        av_collection_release(c);
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
    yyjson_val *points = yyjson_obj_get(root, "points");
    yyjson_val *filter = yyjson_obj_get(root, "filter");
    const av_coldef *def = av_collection_def(c);
    av_store *store = av_rest_get_store();
    av_tx_response out;
    memset(&out, 0, sizeof(out));
    av_status st = AV_OK;

    if (yyjson_is_arr(points)) {
        size_t n = yyjson_arr_size(points);
        if (n > 1000) {
            av_err_set(&e, "invalid_argument", "at most 1000 point ids per delete");
            st = AV_ERR;
        }
        const char **refs =
            st == AV_OK ? av_arena_alloc(req->arena, (n > 0 ? n : 1) * sizeof(char *), 8) : NULL;
        size_t n_refs = 0;
        size_t i, max;
        yyjson_val *v;
        if (st == AV_OK && refs == NULL) {
            av_err_set(&e, "internal", "out of memory");
            st = AV_ERR;
        }
        if (st == AV_OK) {
            yyjson_arr_foreach(points, i, max, v)
            {
                char idbuf[AV_QDRANT_ID_MAX];
                if (av_qdrant_point_id(v, idbuf, &e) != AV_OK) {
                    st = AV_ERR;
                    break;
                }
                /* Qdrant ignores ids that do not exist: resolve them first, delete only live ones */
                uint8_t key[32];
                bool found = false;
                if (store != NULL &&
                    av_store_get_id(store, def->name, idbuf, strlen(idbuf), key, &found, &e) == AV_OK &&
                    found) {
                    av_collection_read_lock(c);
                    bool live = av_collection_has_key(c, key);
                    av_collection_read_unlock(c);
                    if (live)
                        refs[n_refs++] = av_arena_strdup(req->arena, idbuf);
                }
                av_err_clear(&e);
            }
        }
        if (st == AV_OK && n_refs > 0)
            st = av_writer_delete_refs(w, c, refs, n_refs, req->arena, &out, &e);
    } else if (filter != NULL && !yyjson_is_null(filter)) {
        av_filter *ast = NULL;
        st = qdrant_compile_filter(filter, def, req->arena, &ast, &e);
        roaring_bitmap_t *cand = NULL;
        if (st == AV_OK && ast != NULL) {
            av_collection_read_lock(c);
            cand = av_filter_eval(ast, av_collection_attr_index(c), av_collection_docmap(c),
                                  av_collection_live(c), av_collection_as_of(c), &e);
            if (cand == NULL && ast->kind == AV_F_TRUE)
                cand = roaring_bitmap_copy(av_collection_live(c));
            av_collection_read_unlock(c);
            if (cand == NULL)
                st = AV_ERR;
        }
        if (st == AV_OK && ast == NULL) {
            av_err_set(&e, "invalid_argument", "an empty filter would delete every point; refusing");
            st = AV_ERR;
        }
        if (st == AV_OK)
            st = av_writer_delete_filter(w, c, cand, req->arena, &out, &e);
        if (cand != NULL)
            roaring_bitmap_free(cand);
    } else {
        av_err_set(&e, "invalid_argument", "body needs \"points\" (array of ids) or \"filter\"");
        st = AV_ERR;
    }
    yyjson_doc_free(req_doc);

    bool wait = qdrant_wait(req);
    if (st == AV_OK && wait && out.block_number > 0)
        st = av_collection_wait_block(c, out.block_number, 60000, &e);
    av_collection_release(c);
    if (st != AV_OK) {
        qdrant_error_err(resp, &e, t0);
        return;
    }
    qdrant_update_result(resp, wait, t0);
}

/* Endpoints of the contract table not implemented yet: a Qdrant-shaped 501, never a fake answer. */
static void h_qdrant_not_implemented(const av_http_req *req, const char *const *params, size_t n_params,
                                     av_http_resp *resp, void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;
    char msg[256];
    snprintf(msg, sizeof msg, "not_implemented: %s %.160s is not supported by the Qdrant compatibility layer",
             req->method, req->path);
    qdrant_error(resp, 501, msg, time_sec());
}

/* GET /qdrant and /qdrant/: server info (qdrant-client checks the version at startup). */
static void h_qdrant_root(const av_http_req *req, const char *const *params, size_t n_params,
                          av_http_resp *resp, void *ctx)
{
    (void) req;
    (void) params;
    (void) n_params;
    (void) ctx;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "title",
                           "qdrant - vector search engine (arkivector compatibility layer)");
    yyjson_mut_obj_add_str(doc, root, "version", AV_QDRANT_COMPAT_VERSION);
    yyjson_mut_obj_add_strcpy(doc, root, "arkivector", av_version_string());
    resp->status = 200;
    resp->content_type = "application/json";
    size_t len = 0;
    resp->body = yyjson_mut_write(doc, 0, &len);
    resp->body_len = len;
    yyjson_mut_doc_free(doc);
}

void av_rest_register_qdrant(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;
    av_err e;
    av_err_clear(&e);

    (void) av_router_add(r, "GET", "/qdrant", h_qdrant_root, NULL, &e);
    (void) av_router_add(r, "GET", "/qdrant/collections", h_qdrant_list_collections, NULL, &e);
    (void) av_router_add(r, "GET", "/qdrant/collections/{name}", h_qdrant_get_collection, NULL, &e);
    (void) av_router_add(r, "PUT", "/qdrant/collections/{name}", h_qdrant_create_collection, NULL, &e);
    (void) av_router_add(r, "DELETE", "/qdrant/collections/{name}", h_qdrant_delete_collection, NULL, &e);
    (void) av_router_add(r, "PUT", "/qdrant/collections/{name}/points", h_qdrant_upsert_points, NULL, &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/search", h_qdrant_search_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/query", h_qdrant_query_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/count", h_qdrant_count_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/delete", h_qdrant_delete_points, NULL,
                         &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points", h_qdrant_not_implemented, NULL, &e);
    (void) av_router_add(r, "POST", "/qdrant/collections/{name}/points/scroll", h_qdrant_not_implemented,
                         NULL, &e);
}
