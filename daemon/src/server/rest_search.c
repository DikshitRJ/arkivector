/*
 * server/rest_search.c: REST search endpoints (/v1/collections/{name}/search, scroll, count,
 * debug routes).
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "codec/base64.h"
#include "codec/hex.h"
#include "codec/text.h"
#include "codec/vectorbytes.h"
#include "codec/vtype.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/eval.h"
#include "filter/json_filter.h"
#include "filter/parser.h"
#include "filter/value.h"
#include "filter/value_json.h"
#include "kernels/opclass.h"
#include "query/calibrate.h"
#include "query/executor.h"
#include "query/planner.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/rest.h"
#include "store/lmdb_store.h"
#include "store/schema.h"
#include "yyjson.h"

static void fail_internal(av_http_resp *resp, const char *msg, const char *instance)
{
    av_err e;
    av_err_set(&e, "internal", "%s", msg);
    av_problem_from_err(&e, resp, instance);
}

static uint64_t hash_str(const char *s, size_t len)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t) s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static av_status parse_query_vector(yyjson_val *vec_val, yyjson_val *vb_val, const av_coldef *coldef,
                                    av_arena *arena, void **out_pgvec, av_err *e)
{
    if (vb_val != NULL) {
        const char *b64 = yyjson_get_str(vb_val);
        if (b64 == NULL) {
            av_err_set(e, "invalid_argument", "vector_bytes must be a string");
            av_err_field(e, "/vector_bytes");
            return AV_ERR;
        }

        uint8_t *vb_buf = NULL;
        size_t vb_len = 0;
        if (av_base64_decode(b64, strlen(b64), arena, &vb_buf, &vb_len, e) != AV_OK) {
            av_err_set(e, "invalid_vector", "invalid base64 in vector_bytes");
            av_err_field(e, "/vector_bytes");
            return AV_ERR;
        }

        av_vb_info info;
        if (av_vb_decode(vb_buf, vb_len, arena, out_pgvec, &info, e) != AV_OK) {
            av_err_field(e, "/vector_bytes");
            return AV_ERR;
        }

        if (info.type != coldef->vtype) {
            av_err_set(e, "invalid_vector", "vector type mismatch: query is %s, collection is %s",
                       av_vtype_name(info.type), av_vtype_name(coldef->vtype));
            av_err_field(e, "/vector_bytes");
            return AV_ERR;
        }

        if (info.dim != coldef->dimensions) {
            av_err_set(e, "dimension_mismatch", "expected %" PRId64 " dimensions, not %d", coldef->dimensions,
                       info.dim);
            av_err_field(e, "/vector_bytes");
            return AV_ERR;
        }

        return AV_OK;
    }

    /* vec_val != NULL */
    if (yyjson_is_str(vec_val)) {
        const char *s = yyjson_get_str(vec_val);
        if (av_text_to_pgvec(coldef->vtype, s, (int32_t) coldef->dimensions, arena, out_pgvec, e) != AV_OK) {
            av_err_field(e, "/vector");
            return AV_ERR;
        }
        return AV_OK;
    }

    if (yyjson_is_arr(vec_val)) {
        size_t n = yyjson_arr_size(vec_val);
        if (n != (size_t) coldef->dimensions) {
            av_err_set(e, "dimension_mismatch", "expected %" PRId64 " dimensions, not %zu",
                       coldef->dimensions, n);
            av_err_field(e, "/vector");
            return AV_ERR;
        }

        if (coldef->vtype == AV_VT_BIT) {
            char *bit_str = av_arena_alloc(arena, n + 1, 1);
            if (bit_str == NULL) {
                av_err_set(e, "internal", "out of memory");
                return AV_ERR;
            }
            size_t idx, max;
            yyjson_val *elem;
            yyjson_arr_foreach(vec_val, idx, max, elem)
            {
                if (!yyjson_is_int(elem)) {
                    av_err_set(e, "invalid_vector", "bit vector element must be 0 or 1");
                    av_err_field(e, "/vector");
                    return AV_ERR;
                }
                int v = yyjson_get_int(elem);
                if (v != 0 && v != 1) {
                    av_err_set(e, "invalid_vector", "bit vector element must be 0 or 1");
                    av_err_field(e, "/vector");
                    return AV_ERR;
                }
                bit_str[idx] = (char) ('0' + v);
            }
            bit_str[n] = '\0';
            if (av_text_to_pgvec(AV_VT_BIT, bit_str, (int32_t) coldef->dimensions, arena, out_pgvec, e) !=
                AV_OK) {
                av_err_field(e, "/vector");
                return AV_ERR;
            }
            return AV_OK;
        }

        /* Dense vector/halfvec array */
        size_t cap = n * 32 + 16;
        char *buf = av_arena_alloc(arena, cap, 1);
        if (buf == NULL) {
            av_err_set(e, "internal", "out of memory");
            return AV_ERR;
        }

        size_t pos = 0;
        buf[pos++] = '[';
        size_t idx, max;
        yyjson_val *elem;
        yyjson_arr_foreach(vec_val, idx, max, elem)
        {
            if (!yyjson_is_num(elem)) {
                av_err_set(e, "invalid_vector", "vector element must be a number");
                av_err_field(e, "/vector");
                return AV_ERR;
            }
            double val = yyjson_get_num(elem);
            if (!isfinite(val)) {
                av_err_set(e, "invalid_vector", "vector element must be finite");
                av_err_field(e, "/vector");
                return AV_ERR;
            }
            int written = snprintf(buf + pos, cap - pos, "%s%.9g", (idx > 0 ? "," : ""), val);
            if (written < 0 || (size_t) written >= cap - pos) {
                av_err_set(e, "internal", "buffer overflow formatting vector");
                return AV_ERR;
            }
            pos += (size_t) written;
        }
        buf[pos++] = ']';
        buf[pos] = '\0';

        if (av_text_to_pgvec(coldef->vtype, buf, (int32_t) coldef->dimensions, arena, out_pgvec, e) !=
            AV_OK) {
            av_err_field(e, "/vector");
            return AV_ERR;
        }
        return AV_OK;
    }

    if (yyjson_is_obj(vec_val)) {
        /* Sparse vector object: {dim, indices, values} */
        int64_t dim = 0;
        bool p = false;
        if (av_json_get_int(vec_val, "dim", "/vector", true, 1, 1000000000, &dim, &p, e) != AV_OK) {
            return AV_ERR;
        }
        if (dim != coldef->dimensions) {
            av_err_set(e, "dimension_mismatch", "expected %" PRId64 " dimensions, not %" PRId64,
                       coldef->dimensions, dim);
            av_err_field(e, "/vector");
            return AV_ERR;
        }

        yyjson_val *idx_arr = yyjson_obj_get(vec_val, "indices");
        yyjson_val *val_arr = yyjson_obj_get(vec_val, "values");
        if (!yyjson_is_arr(idx_arr) || !yyjson_is_arr(val_arr)) {
            av_err_set(e, "invalid_argument", "indices and values must be arrays");
            av_err_field(e, "/vector");
            return AV_ERR;
        }
        size_t nnz = yyjson_arr_size(idx_arr);
        if (nnz != yyjson_arr_size(val_arr)) {
            av_err_set(e, "invalid_argument", "indices and values array lengths must match");
            av_err_field(e, "/vector");
            return AV_ERR;
        }

        size_t cap = nnz * 48 + 32;
        char *buf = av_arena_alloc(arena, cap, 1);
        if (buf == NULL) {
            av_err_set(e, "internal", "out of memory");
            return AV_ERR;
        }

        size_t pos = 0;
        buf[pos++] = '{';
        for (size_t i = 0; i < nnz; i++) {
            yyjson_val *iv = yyjson_arr_get(idx_arr, i);
            yyjson_val *vv = yyjson_arr_get(val_arr, i);
            if (!yyjson_is_int(iv) || !yyjson_is_num(vv)) {
                av_err_set(e, "invalid_vector", "sparse element invalid type");
                av_err_field(e, "/vector");
                return AV_ERR;
            }
            int64_t index0 = yyjson_get_int(iv);
            double val = yyjson_get_num(vv);
            /* pgvector sparse text format uses 1-based indexing */
            int written =
                snprintf(buf + pos, cap - pos, "%s%" PRId64 ":%.9g", (i > 0 ? "," : ""), index0 + 1, val);
            if (written < 0 || (size_t) written >= cap - pos) {
                av_err_set(e, "internal", "buffer overflow formatting sparse vector");
                return AV_ERR;
            }
            pos += (size_t) written;
        }
        int written = snprintf(buf + pos, cap - pos, "}/%" PRId64, dim);
        if (written < 0 || (size_t) written >= cap - pos) {
            av_err_set(e, "internal", "buffer overflow formatting sparse vector");
            return AV_ERR;
        }

        if (av_text_to_pgvec(AV_VT_SPARSEVEC, buf, (int32_t) coldef->dimensions, arena, out_pgvec, e) !=
            AV_OK) {
            av_err_field(e, "/vector");
            return AV_ERR;
        }
        return AV_OK;
    }

    av_err_set(e, "invalid_argument", "vector must be an array, string, or sparse object");
    av_err_field(e, "/vector");
    return AV_ERR;
}

/* Parse consistency object and wait if needed */
static av_status handle_consistency(yyjson_val *root, av_collection *c, const char *path, av_http_resp *resp,
                                    av_err *e)
{
    yyjson_val *cons = yyjson_obj_get(root, "consistency");
    if (cons == NULL || yyjson_is_null(cons)) {
        return AV_OK;
    }

    if (!yyjson_is_obj(cons)) {
        av_err_set(e, "bad_request", "consistency must be an object");
        av_err_field(e, "/consistency");
        av_problem_from_err(e, resp, path);
        return AV_ERR;
    }

    static const char *const allowed_cons[] = {"min_block", "timeout_ms"};
    if (av_json_check_fields(cons, allowed_cons, 2, "/consistency", e) != AV_OK) {
        av_problem_from_err(e, resp, path);
        return AV_ERR;
    }

    yyjson_val *mb_val = yyjson_obj_get(cons, "min_block");
    if (mb_val != NULL && !yyjson_is_null(mb_val)) {
        if (!yyjson_is_int(mb_val)) {
            av_err_set(e, "bad_request", "consistency.min_block must be an integer");
            av_err_field(e, "/consistency/min_block");
            av_problem_from_err(e, resp, path);
            return AV_ERR;
        }
        uint64_t min_block = (uint64_t) yyjson_get_uint(mb_val);

        int timeout_ms = 5000;
        yyjson_val *t_val = yyjson_obj_get(cons, "timeout_ms");
        if (t_val != NULL && !yyjson_is_null(t_val)) {
            if (!yyjson_is_int(t_val)) {
                av_err_set(e, "bad_request", "consistency.timeout_ms must be an integer");
                av_err_field(e, "/consistency/timeout_ms");
                av_problem_from_err(e, resp, path);
                return AV_ERR;
            }
            timeout_ms = yyjson_get_int(t_val);
            if (timeout_ms < 0)
                timeout_ms = 0;
            if (timeout_ms > 60000)
                timeout_ms = 60000;
        }

        if (av_collection_wait_block(c, min_block, timeout_ms, e) != AV_OK) {
            av_problem_from_err(e, resp, path);
            return AV_ERR;
        }
    } else if (av_collection_is_bootstrapping(c)) {
        av_err_set(e, "not_ready", "collection is bootstrapping; retry shortly");
        resp->retry_after = "1";
        av_problem_from_err(e, resp, path);
        return AV_ERR;
    }
    return AV_OK;
}

/* Parse filter / where into an AST */
static av_status parse_filter_or_where(yyjson_val *root, const av_coldef *coldef, av_arena *arena,
                                       av_filter **out_ast, uint64_t *out_hash, av_err *e)
{
    *out_ast = NULL;
    *out_hash = 0;

    yyjson_val *filt_val = yyjson_obj_get(root, "filter");
    yyjson_val *where_val = yyjson_obj_get(root, "where");

    if (filt_val != NULL && where_val != NULL) {
        av_err_set(e, "invalid_argument", "cannot provide both 'filter' and 'where'");
        av_err_field(e, "/filter");
        return AV_ERR;
    }

    if (filt_val != NULL && !yyjson_is_null(filt_val)) {
        if (!yyjson_is_obj(filt_val)) {
            av_err_set(e, "bad_request", "filter must be an object");
            av_err_field(e, "/filter");
            return AV_ERR;
        }
        size_t flen = 0;
        char *fstr = yyjson_val_write(filt_val, 0, &flen);
        if (fstr != NULL) {
            *out_hash = hash_str(fstr, flen);
            free(fstr);
        }
        return av_filter_from_json(filt_val, coldef, arena, out_ast, e);
    }

    if (where_val != NULL && !yyjson_is_null(where_val)) {
        if (!yyjson_is_str(where_val)) {
            av_err_set(e, "bad_request", "where must be a string");
            av_err_field(e, "/where");
            return AV_ERR;
        }
        const char *w = yyjson_get_str(where_val);
        size_t wlen = yyjson_get_len(where_val);
        *out_hash = hash_str(w, wlen);
        return av_filter_from_where(w, wlen, coldef, arena, out_ast, e);
    }

    return AV_OK;
}

/* POST /v1/collections/{name}/search */
static void h_search(const av_http_req *req, const char *const *params, size_t n_params, av_http_resp *resp,
                     void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_collection *c = av_registry_get(reg, coll_name);
    if (c == NULL) {
        av_err e;
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const av_coldef *coldef = av_collection_def(c);

    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        av_collection_release(c);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = av_json_root_object(req_doc, &e);
    if (root == NULL) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    static const char *const allowed_search[] = {
        "vector",       "vector_bytes", "k",         "offset",         "operator",
        "filter",       "where",        "ef_search", "iterative_scan", "max_scan_tuples",
        "max_distance", "min_score",    "exact",     "include",        "consistency",
        "explain"};
    if (av_json_check_fields(root, allowed_search, sizeof(allowed_search) / sizeof(allowed_search[0]), "",
                             &e) != AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *vec_val = yyjson_obj_get(root, "vector");
    yyjson_val *vb_val = yyjson_obj_get(root, "vector_bytes");

    if ((vec_val != NULL && vb_val != NULL) || (vec_val == NULL && vb_val == NULL)) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_err_set(&e, "invalid_argument", "must provide exactly one of 'vector' or 'vector_bytes'");
        av_err_field(&e, "/vector");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Consistency wait */
    if (handle_consistency(root, c, req->path, resp, &e) != AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        return;
    }

    /* Decode query vector */
    void *query_vec = NULL;
    if (parse_query_vector(vec_val, vb_val, coldef, req->arena, &query_vec, &e) != AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* k */
    size_t k = 10;
    yyjson_val *k_val = yyjson_obj_get(root, "k");
    if (k_val != NULL) {
        if (!yyjson_is_int(k_val)) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "bad_request", "k must be an integer");
            av_err_field(&e, "/k");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        int64_t kv = yyjson_get_int(k_val);
        if (kv < 1 || kv > 1000) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "invalid_argument", "k must be in 1..1000");
            av_err_field(&e, "/k");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        k = (size_t) kv;
    }

    /* offset */
    size_t offset = 0;
    yyjson_val *offset_val = yyjson_obj_get(root, "offset");
    if (offset_val != NULL) {
        if (!yyjson_is_int(offset_val)) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "bad_request", "offset must be an integer");
            av_err_field(&e, "/offset");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        int64_t ov = yyjson_get_int(offset_val);
        if (ov < 0 || ov > 999) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "invalid_argument", "offset must be in 0..999");
            av_err_field(&e, "/offset");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        offset = (size_t) ov;
    }

    if (offset + k > 1000) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_err_set(&e, "invalid_argument", "offset + k must be <= 1000");
        av_err_field(&e, "/offset");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* operator */
    const av_opclass *oc = coldef->opclass;
    yyjson_val *op_val = yyjson_obj_get(root, "operator");
    if (op_val != NULL) {
        if (!yyjson_is_str(op_val)) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "bad_request", "operator must be a string");
            av_err_field(&e, "/operator");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        const char *op_str = yyjson_get_str(op_val);
        const av_opclass *found_oc = av_opclass_for_operator(coldef->vtype, op_str);
        if (found_oc == NULL) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "invalid_argument", "operator '%s' is not valid for column type '%s'", op_str,
                       av_vtype_name(coldef->vtype));
            av_err_field(&e, "/operator");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        oc = found_oc;
    }

    /* include */
    int include_flags = AV_INCLUDE_ATTRIBUTES;
    yyjson_val *inc_val = yyjson_obj_get(root, "include");
    if (inc_val != NULL) {
        if (!yyjson_is_arr(inc_val)) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_err_set(&e, "bad_request", "include must be an array of strings");
            av_err_field(&e, "/include");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        include_flags = 0;
        size_t idx, max;
        yyjson_val *elem;
        yyjson_arr_foreach(inc_val, idx, max, elem)
        {
            const char *is = yyjson_get_str(elem);
            if (is != NULL) {
                if (strcmp(is, "attributes") == 0) {
                    include_flags |= AV_INCLUDE_ATTRIBUTES;
                } else if (strcmp(is, "document") == 0) {
                    include_flags |= AV_INCLUDE_DOCUMENT;
                } else if (strcmp(is, "vector") == 0) {
                    include_flags |= AV_INCLUDE_VECTOR;
                }
            }
        }
    }

    /* exact, explain, max_distance, min_score */
    bool exact = false;
    yyjson_val *ex_val = yyjson_obj_get(root, "exact");
    if (ex_val != NULL && yyjson_is_bool(ex_val)) {
        exact = yyjson_get_bool(ex_val);
    }

    bool explain = false;
    yyjson_val *exp_val = yyjson_obj_get(root, "explain");
    if (exp_val != NULL && yyjson_is_bool(exp_val)) {
        explain = yyjson_get_bool(exp_val);
    }

    bool has_max_dist = false;
    double max_dist = 0.0;
    yyjson_val *md_val = yyjson_obj_get(root, "max_distance");
    if (md_val != NULL && yyjson_is_num(md_val)) {
        has_max_dist = true;
        max_dist = yyjson_get_num(md_val);
    }

    bool has_min_sc = false;
    double min_sc = 0.0;
    yyjson_val *ms_val = yyjson_obj_get(root, "min_score");
    if (ms_val != NULL && yyjson_is_num(ms_val)) {
        has_min_sc = true;
        min_sc = yyjson_get_num(ms_val);
    }

    /* Parse filter or where */
    av_filter *ast = NULL;
    uint64_t filt_hash = 0;
    if (parse_filter_or_where(root, coldef, req->arena, &ast, &filt_hash, &e) != AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }
    yyjson_doc_free(req_doc);

    /* Search parameters */
    av_search_params sparams;
    memset(&sparams, 0, sizeof(sparams));
    sparams.query_vec = query_vec;
    sparams.oc = oc;
    sparams.k = k;
    sparams.offset = offset;
    sparams.has_max_distance = has_max_dist;
    sparams.max_distance = max_dist;
    sparams.has_min_score = has_min_sc;
    sparams.min_score = min_sc;
    sparams.exact = exact;
    sparams.include_flags = include_flags;
    sparams.explain = explain;

    /* Execute search under read lock */
    av_collection_read_lock(c);
    uint64_t as_of = av_collection_as_of(c);
    const roaring_bitmap_t *live = av_collection_live(c);
    const av_docmap *docmap = av_collection_docmap(c);
    const av_attr_index *ix = av_collection_attr_index(c);

    roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, live, as_of, &e);
    if (cand == NULL && ast != NULL && ast->kind != AV_F_TRUE) {
        av_collection_read_unlock(c);
        av_collection_release(c);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_store *store = av_rest_get_store();
    av_search_result sres;
    memset(&sres, 0, sizeof(sres));

    av_status st = av_execute_search(c, store, &sparams, cand, &sres, &e);

    /* Recall canary: sample 1% of approximate searches and measure recall@k */
    if (st == AV_OK && !sparams.exact && !sres.plan.exact && sres.n_hits > 0) {
        if ((rand() % 100) == 0) {
            av_search_params exact_params = sparams;
            exact_params.exact = true;
            exact_params.explain = false;
            av_search_result exact_res;
            memset(&exact_res, 0, sizeof(exact_res));
            av_err err_exact;
            av_err_clear(&err_exact);
            if (av_execute_search(c, store, &exact_params, cand, &exact_res, &err_exact) == AV_OK) {
                size_t matches = 0;
                for (size_t i = 0; i < sres.n_hits; i++) {
                    for (size_t j = 0; j < exact_res.n_hits; j++) {
                        if (memcmp(sres.hits[i].key, exact_res.hits[j].key, 32) == 0) {
                            matches++;
                            break;
                        }
                    }
                }
                double recall = exact_res.n_hits > 0 ? ((double) matches / (double) exact_res.n_hits) : 1.0;
                av_recall_canary_record(coldef->name, recall);
                av_search_result_free(&exact_res);
            }
        }
    }

    av_collection_read_unlock(c);

    if (cand != NULL) {
        roaring_bitmap_free(cand);
    }

    if (st != AV_OK) {
        av_collection_release(c);
        av_search_result_free(&sres);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Build response JSON reusing sres.doc so hit attributes belong to it */
    yyjson_mut_doc *doc = sres.doc;
    if (doc == NULL) {
        doc = yyjson_mut_doc_new(NULL);
        sres.doc = doc;
    }

    yyjson_mut_val *root_resp = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_resp);

    yyjson_mut_obj_add_uint(doc, root_resp, "as_of_block", sres.as_of_block);
    yyjson_mut_obj_add_strcpy(doc, root_resp, "sync_state", sres.sync_state);
    yyjson_mut_obj_add_bool(doc, root_resp, "open_collection", sres.open_collection);

    yyjson_mut_val *results = yyjson_mut_arr(doc);
    for (size_t i = 0; i < sres.n_hits; i++) {
        const av_search_hit *h = &sres.hits[i];
        yyjson_mut_val *ho = yyjson_mut_obj(doc);

        char khex[67];
        av_key_to_hex(h->key, khex);
        yyjson_mut_obj_add_strcpy(doc, ho, "key", khex);

        if (h->has_id) {
            yyjson_mut_obj_add_strcpy(doc, ho, "id", h->id);
        } else {
            yyjson_mut_obj_add_null(doc, ho, "id");
        }

        yyjson_mut_obj_add_real(doc, ho, "distance", h->distance);
        yyjson_mut_obj_add_real(doc, ho, "score", h->score);

        char addr[43];
        av_addr_to_hex(h->owner, addr);
        yyjson_mut_obj_add_strcpy(doc, ho, "owner", addr);

        av_addr_to_hex(h->creator, addr);
        yyjson_mut_obj_add_strcpy(doc, ho, "creator", addr);

        yyjson_mut_obj_add_uint(doc, ho, "expires_at_block", h->expires_at);

        if (h->attributes_obj != NULL) {
            yyjson_mut_obj_add_val(doc, ho, "attributes", h->attributes_obj);
        } else {
            yyjson_mut_val *empty_at = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, ho, "attributes", empty_at);
        }

        if (h->document != NULL) {
            yyjson_mut_obj_add_strncpy(doc, ho, "document", h->document, h->doc_len);
        } else {
            yyjson_mut_obj_add_null(doc, ho, "document");
        }

        if (h->vector_bytes != NULL) {
            yyjson_mut_obj_add_strcpy(doc, ho, "vector_bytes", h->vector_bytes);
        }

        yyjson_mut_arr_append(results, ho);
    }
    yyjson_mut_obj_add_val(doc, root_resp, "results", results);

    if (explain) {
        yyjson_mut_val *plan_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, plan_obj, "strategy", av_plan_strategy_name(sres.plan.strategy));
        yyjson_mut_obj_add_bool(doc, plan_obj, "exact", sres.plan.exact);
        yyjson_mut_obj_add_real(doc, plan_obj, "elapsed_ms", sres.plan.elapsed_ms);
        if (sres.plan.candidates > 0 || ast != NULL) {
            yyjson_mut_obj_add_uint(doc, plan_obj, "candidates", sres.plan.candidates);
            yyjson_mut_obj_add_real(doc, plan_obj, "selectivity", sres.plan.selectivity);
        } else {
            yyjson_mut_obj_add_null(doc, plan_obj, "candidates");
            yyjson_mut_obj_add_null(doc, plan_obj, "selectivity");
        }
        if (sres.plan.kernel != NULL) {
            yyjson_mut_obj_add_strcpy(doc, plan_obj, "kernel", sres.plan.kernel);
        }
        yyjson_mut_obj_add_val(doc, root_resp, "plan", plan_obj);
    }

    resp->has_as_of = true;
    resp->as_of_block = sres.as_of_block;

    /* av_resp_json frees doc; clear sres.doc so av_search_result_free won't double-free */
    sres.doc = NULL;
    (void) av_resp_json(resp, 200, doc);

    av_search_result_free(&sres);
    av_collection_release(c);
}

/* POST /v1/collections/{name}/scroll */
typedef struct {
    uint8_t key[32];
    av_docid docid;
} scroll_cand;

static int scroll_cand_cmp(const void *a, const void *b)
{
    return memcmp(((const scroll_cand *) a)->key, ((const scroll_cand *) b)->key, 32);
}

typedef struct {
    const av_docmap *docmap;
    scroll_cand *items;
    size_t count;
    size_t cap;
    bool oom;
} scroll_collector;

static bool scroll_collect_cb(uint32_t docid, void *ptr)
{
    scroll_collector *acc = (scroll_collector *) ptr;
    const uint8_t *key = av_docmap_key(acc->docmap, (av_docid) docid);
    if (key == NULL) {
        return true;
    }
    if (acc->count >= acc->cap) {
        size_t ncap = acc->cap == 0 ? 128 : acc->cap * 2;
        scroll_cand *nitems = realloc(acc->items, ncap * sizeof(scroll_cand));
        if (nitems == NULL) {
            acc->oom = true;
            return false;
        }
        acc->items = nitems;
        acc->cap = ncap;
    }
    memcpy(acc->items[acc->count].key, key, 32);
    acc->items[acc->count].docid = (av_docid) docid;
    acc->count++;
    return true;
}

static void h_scroll(const av_http_req *req, const char *const *params, size_t n_params, av_http_resp *resp,
                     void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_collection *c = av_registry_get(reg, coll_name);
    if (c == NULL) {
        av_err e;
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const av_coldef *coldef = av_collection_def(c);

    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = NULL;
    if (req->body_len > 0) {
        if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
            av_collection_release(c);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    }

    yyjson_val *root = req_doc != NULL ? av_json_root_object(req_doc, &e) : NULL;
    if (req_doc != NULL && root == NULL) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    if (root != NULL) {
        static const char *const allowed_scroll[] = {"filter", "where",   "limit",
                                                     "cursor", "include", "consistency"};
        if (av_json_check_fields(root, allowed_scroll, sizeof(allowed_scroll) / sizeof(allowed_scroll[0]), "",
                                 &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        if (handle_consistency(root, c, req->path, resp, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            return;
        }
    }

    size_t limit = 100;
    if (root != NULL) {
        yyjson_val *lim_val = yyjson_obj_get(root, "limit");
        if (lim_val != NULL && !yyjson_is_null(lim_val)) {
            if (!yyjson_is_int(lim_val)) {
                av_collection_release(c);
                yyjson_doc_free(req_doc);
                av_err_set(&e, "bad_request", "limit must be an integer");
                av_err_field(&e, "/limit");
                av_problem_from_err(&e, resp, req->path);
                return;
            }
            int64_t lv = yyjson_get_int(lim_val);
            if (lv < 1 || lv > 1000) {
                av_collection_release(c);
                yyjson_doc_free(req_doc);
                av_err_set(&e, "invalid_argument", "limit must be in 1..1000");
                av_err_field(&e, "/limit");
                av_problem_from_err(&e, resp, req->path);
                return;
            }
            limit = (size_t) lv;
        }
    }

    int include_flags = AV_INCLUDE_ATTRIBUTES;
    if (root != NULL) {
        yyjson_val *inc_val = yyjson_obj_get(root, "include");
        if (inc_val != NULL && !yyjson_is_null(inc_val)) {
            include_flags = 0;
            size_t idx, max;
            yyjson_val *elem;
            yyjson_arr_foreach(inc_val, idx, max, elem)
            {
                const char *is = yyjson_get_str(elem);
                if (is != NULL) {
                    if (strcmp(is, "attributes") == 0) {
                        include_flags |= AV_INCLUDE_ATTRIBUTES;
                    } else if (strcmp(is, "document") == 0) {
                        include_flags |= AV_INCLUDE_DOCUMENT;
                    } else if (strcmp(is, "vector") == 0) {
                        include_flags |= AV_INCLUDE_VECTOR;
                    }
                }
            }
        }
    }

    /* Parse cursor if given */
    bool has_cursor = false;
    uint8_t cursor_key[32];
    memset(cursor_key, 0, sizeof(cursor_key));

    if (root != NULL) {
        yyjson_val *cur_val = yyjson_obj_get(root, "cursor");
        if (cur_val != NULL && !yyjson_is_null(cur_val)) {
            const char *cur_str = yyjson_get_str(cur_val);
            if (cur_str == NULL) {
                av_collection_release(c);
                yyjson_doc_free(req_doc);
                av_err_set(&e, "bad_request", "cursor must be a string");
                av_err_field(&e, "/cursor");
                av_problem_from_err(&e, resp, req->path);
                return;
            }

            uint8_t *cbuf = NULL;
            size_t clen = 0;
            if (av_base64_decode(cur_str, strlen(cur_str), req->arena, &cbuf, &clen, &e) != AV_OK) {
                av_collection_release(c);
                yyjson_doc_free(req_doc);
                av_err_set(&e, "invalid_argument", "invalid base64 in cursor");
                av_err_field(&e, "/cursor");
                av_problem_from_err(&e, resp, req->path);
                return;
            }

            char ctext[256];
            size_t copy_len = clen < sizeof(ctext) - 1 ? clen : sizeof(ctext) - 1;
            memcpy(ctext, cbuf, copy_len);
            ctext[copy_len] = '\0';

            /* Format: v1|<as_of>|<last_key_hex>[|<filter_hash>]. Only the key is used to resume. */
            const char *p1 = strchr(ctext, '|');
            const char *p2 = p1 != NULL ? strchr(p1 + 1, '|') : NULL;
            if (p1 != NULL && p2 != NULL && (size_t) (p1 - ctext) == 2 && strncmp(ctext, "v1", 2) == 0) {
                const char *kbeg = p2 + 1;
                const char *kend = strchr(kbeg, '|');
                size_t klen = kend != NULL ? (size_t) (kend - kbeg) : strlen(kbeg);
                char key_hex[67];
                if (klen == 66) {
                    memcpy(key_hex, kbeg, 66);
                    key_hex[66] = '\0';
                    if (av_key_from_hex(key_hex, cursor_key)) {
                        has_cursor = true;
                    }
                }
            }

            if (!has_cursor) {
                av_collection_release(c);
                yyjson_doc_free(req_doc);
                av_err_set(&e, "invalid_argument", "invalid cursor format");
                av_err_field(&e, "/cursor");
                av_problem_from_err(&e, resp, req->path);
                return;
            }
        }
    }

    av_filter *ast = NULL;
    uint64_t filter_hash = 0;
    if (root != NULL) {
        if (parse_filter_or_where(root, coldef, req->arena, &ast, &filter_hash, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    }
    if (req_doc != NULL) {
        yyjson_doc_free(req_doc);
    }

    /* Evaluate filter under read lock */
    av_collection_read_lock(c);
    uint64_t as_of = av_collection_as_of(c);
    const roaring_bitmap_t *live = av_collection_live(c);
    const av_docmap *docmap = av_collection_docmap(c);
    const av_attr_index *ix = av_collection_attr_index(c);

    roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, live, as_of, &e);
    if (cand == NULL && ast != NULL && ast->kind != AV_F_TRUE) {
        av_collection_read_unlock(c);
        av_collection_release(c);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    scroll_collector sc = {docmap, NULL, 0, 0, false};
    if (cand != NULL) {
        roaring_iterate(cand, scroll_collect_cb, &sc);
        roaring_bitmap_free(cand);
    } else {
        size_t count = av_collection_live_count(c);
        if (count > 0) {
            roaring_iterate(live, scroll_collect_cb, &sc);
        }
    }
    av_collection_read_unlock(c);

    if (sc.oom) {
        free(sc.items);
        av_collection_release(c);
        fail_internal(resp, "out of memory collecting scroll items", req->path);
        return;
    }

    if (sc.count > 1) {
        qsort(sc.items, sc.count, sizeof(scroll_cand), scroll_cand_cmp);
    }

    size_t start_idx = 0;
    if (has_cursor) {
        while (start_idx < sc.count && memcmp(sc.items[start_idx].key, cursor_key, 32) <= 0) {
            start_idx++;
        }
    }

    size_t remaining = sc.count > start_idx ? sc.count - start_idx : 0;
    size_t take = remaining < limit ? remaining : limit;

    av_store *store = av_rest_get_store();
    if (store == NULL) {
        free(sc.items);
        av_collection_release(c);
        fail_internal(resp, "store service not available", req->path);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        free(sc.items);
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }
    yyjson_mut_val *root_resp = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_resp);

    yyjson_mut_obj_add_uint(doc, root_resp, "as_of_block", as_of);
    yyjson_mut_val *ents_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root_resp, "entities", ents_arr);

    for (size_t i = 0; i < take; i++) {
        const uint8_t *k = sc.items[start_idx + i].key;
        av_row row;
        bool found = false;
        if (av_store_get_entity(store, coll_name, k, req->arena, &row, &found, &e) == AV_OK && found) {
            av_attr *attrs = NULL;
            size_t n_attrs = 0;
            if (av_attrs_unpack(row.attrs_blob, row.attrs_len, req->arena, &attrs, &n_attrs, &e) == AV_OK) {
                yyjson_mut_val *eo = yyjson_mut_obj(doc);
                char khex[67];
                av_key_to_hex(k, khex);
                yyjson_mut_obj_add_strcpy(doc, eo, "key", khex);

                const char *id_val = NULL;
                size_t id_len = 0;
                const char *doc_val = NULL;
                size_t doc_len = 0;
                const uint8_t *vec_val = NULL;
                size_t vec_len = 0;

                yyjson_mut_val *attrs_obj = yyjson_mut_obj(doc);
                for (size_t a = 0; a < n_attrs; a++) {
                    const av_attr *at = &attrs[a];
                    if (strcmp(at->name, AV_ATTR_ID) == 0) {
                        id_val = (const char *) at->val;
                        id_len = at->len;
                    } else if (strcmp(at->name, AV_ATTR_DOC) == 0) {
                        doc_val = (const char *) at->val;
                        doc_len = at->len;
                    } else if (strcmp(at->name, AV_ATTR_VEC) == 0) {
                        vec_val = at->val;
                        vec_len = at->len;
                    } else if (strncmp(at->name, "av.", 3) != 0 && strcmp(at->name, AV_ATTR_SCHEMA) != 0) {
                        if (include_flags & AV_INCLUDE_ATTRIBUTES) {
                            yyjson_mut_val *tv = av_attr_to_typed_json(doc, at, req->arena);
                            /* yyjson does not copy keys added with obj_add_val: copy it, the source may be short-lived */
                            {
                                yyjson_mut_val *k_ = yyjson_mut_strcpy(doc, at->name);
                                if (k_ != NULL && tv != NULL)
                                    yyjson_mut_obj_add(attrs_obj, k_, tv);
                            }
                        }
                    }
                }

                if (id_val != NULL) {
                    yyjson_mut_obj_add_strncpy(doc, eo, "id", id_val, id_len);
                } else {
                    yyjson_mut_obj_add_null(doc, eo, "id");
                }

                char addr[43];
                av_addr_to_hex(row.owner, addr);
                yyjson_mut_obj_add_strcpy(doc, eo, "owner", addr);

                av_addr_to_hex(row.creator, addr);
                yyjson_mut_obj_add_strcpy(doc, eo, "creator", addr);

                yyjson_mut_obj_add_uint(doc, eo, "created_at_block", row.created_at);
                yyjson_mut_obj_add_uint(doc, eo, "updated_at_block", row.updated_at);
                yyjson_mut_obj_add_uint(doc, eo, "expires_at_block", row.expires_at);

                yyjson_mut_obj_add_val(doc, eo, "attributes", attrs_obj);

                if ((include_flags & AV_INCLUDE_DOCUMENT) && doc_val != NULL) {
                    yyjson_mut_obj_add_strncpy(doc, eo, "document", doc_val, doc_len);
                } else {
                    yyjson_mut_obj_add_null(doc, eo, "document");
                }

                if ((include_flags & AV_INCLUDE_VECTOR) && vec_val != NULL) {
                    char *b64 = NULL;
                    size_t b64_len = 0;
                    av_err err;
                    av_err_clear(&err);
                    if (av_base64_encode(vec_val, vec_len, req->arena, &b64, &b64_len, &err) == AV_OK) {
                        yyjson_mut_obj_add_strncpy(doc, eo, "vector_bytes", b64, b64_len);
                    }
                }

                yyjson_mut_arr_append(ents_arr, eo);
            }
        }
    }

    /* Next cursor */
    if (start_idx + take < sc.count) {
        const uint8_t *last_k = sc.items[start_idx + take - 1].key;
        char lk_hex[67];
        av_key_to_hex(last_k, lk_hex);

        char token[256];
        (void) snprintf(token, sizeof(token), "v1|%" PRIu64 "|%s|%" PRIx64, as_of, lk_hex, filter_hash);

        char *next_cur = NULL;
        size_t next_cur_len = 0;
        av_err err;
        av_err_clear(&err);
        if (av_base64_encode((const uint8_t *) token, strlen(token), req->arena, &next_cur, &next_cur_len,
                             &err) == AV_OK) {
            yyjson_mut_obj_add_strncpy(doc, root_resp, "next_cursor", next_cur, next_cur_len);
        } else {
            yyjson_mut_obj_add_null(doc, root_resp, "next_cursor");
        }
    } else {
        yyjson_mut_obj_add_null(doc, root_resp, "next_cursor");
    }

    free(sc.items);

    resp->has_as_of = true;
    resp->as_of_block = as_of;
    (void) av_resp_json(resp, 200, doc);
    av_collection_release(c);
}

/* POST /v1/collections/{name}/count */
static void h_count(const av_http_req *req, const char *const *params, size_t n_params, av_http_resp *resp,
                    void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_collection *c = av_registry_get(reg, coll_name);
    if (c == NULL) {
        av_err e;
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const av_coldef *coldef = av_collection_def(c);

    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = NULL;
    if (req->body_len > 0) {
        if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
            av_collection_release(c);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    }

    yyjson_val *root = req_doc != NULL ? av_json_root_object(req_doc, &e) : NULL;
    if (req_doc != NULL && root == NULL) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    if (root != NULL) {
        static const char *const allowed_count[] = {"filter", "where", "consistency"};
        if (av_json_check_fields(root, allowed_count, 3, "", &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        if (handle_consistency(root, c, req->path, resp, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            return;
        }
    }

    av_filter *ast = NULL;
    uint64_t filt_hash = 0;
    if (root != NULL) {
        if (parse_filter_or_where(root, coldef, req->arena, &ast, &filt_hash, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    }
    if (req_doc != NULL) {
        yyjson_doc_free(req_doc);
    }

    av_collection_read_lock(c);
    uint64_t as_of = av_collection_as_of(c);
    const roaring_bitmap_t *live = av_collection_live(c);
    const av_docmap *docmap = av_collection_docmap(c);
    const av_attr_index *ix = av_collection_attr_index(c);

    roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, live, as_of, &e);
    if (cand == NULL && ast != NULL && ast->kind != AV_F_TRUE) {
        av_collection_read_unlock(c);
        av_collection_release(c);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    uint64_t count = 0;
    if (cand != NULL) {
        count = roaring_bitmap_get_cardinality(cand);
        roaring_bitmap_free(cand);
    } else {
        count = av_collection_live_count(c);
    }
    av_collection_read_unlock(c);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }
    yyjson_mut_val *root_resp = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root_resp);

    yyjson_mut_obj_add_uint(doc, root_resp, "as_of_block", as_of);
    yyjson_mut_obj_add_uint(doc, root_resp, "count", count);

    resp->has_as_of = true;
    resp->as_of_block = as_of;
    (void) av_resp_json(resp, 200, doc);
    av_collection_release(c);
}

/* POST /v1/collections/{name}/search:batch */
static void h_search_batch(const av_http_req *req, const char *const *params, size_t n_params,
                           av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_collection *c = av_registry_get(reg, coll_name);
    if (c == NULL) {
        av_err e;
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const av_coldef *coldef = av_collection_def(c);
    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        av_collection_release(c);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = av_json_root_object(req_doc, &e);
    if (root == NULL) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    static const char *const allowed_batch[] = {"searches", "consistency"};
    if (av_json_check_fields(root, allowed_batch, sizeof(allowed_batch) / sizeof(allowed_batch[0]), "", &e) !=
        AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Consistency wait */
    if (handle_consistency(root, c, req->path, resp, &e) != AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        return;
    }

    yyjson_val *searches_arr = yyjson_obj_get(root, "searches");
    if (searches_arr == NULL || !yyjson_is_arr(searches_arr)) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_err_set(&e, "invalid_argument", "'searches' must be an array");
        av_err_field(&e, "/searches");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    size_t n_searches = yyjson_arr_size(searches_arr);
    if (n_searches < 1 || n_searches > 64) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        av_err_set(&e, "invalid_argument", "searches array size must be in 1..64");
        av_err_field(&e, "/searches");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_store *store = av_rest_get_store();
    av_collection_read_lock(c);
    uint64_t as_of = av_collection_as_of(c);
    const roaring_bitmap_t *live = av_collection_live(c);
    const av_docmap *docmap = av_collection_docmap(c);
    const av_attr_index *ix = av_collection_attr_index(c);

    yyjson_mut_doc *resp_doc = yyjson_mut_doc_new(NULL);
    if (resp_doc == NULL) {
        av_collection_read_unlock(c);
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        fail_internal(resp, "out of memory", req->path);
        return;
    }
    yyjson_mut_val *resp_root = yyjson_mut_obj(resp_doc);
    yyjson_mut_doc_set_root(resp_doc, resp_root);

    yyjson_mut_obj_add_uint(resp_doc, resp_root, "as_of_block", as_of);
    yyjson_mut_obj_add_strcpy(resp_doc, resp_root, "sync_state", as_of == 0 ? "bootstrap" : "live");
    yyjson_mut_val *resp_arr = yyjson_mut_arr(resp_doc);

    size_t sidx, smax;
    yyjson_val *sitem;
    yyjson_arr_foreach(searches_arr, sidx, smax, sitem)
    {
        yyjson_val *vec_val = yyjson_obj_get(sitem, "vector");
        yyjson_val *vb_val = yyjson_obj_get(sitem, "vector_bytes");

        void *query_vec = NULL;
        if (parse_query_vector(vec_val, vb_val, coldef, req->arena, &query_vec, &e) != AV_OK) {
            continue;
        }

        size_t k = 10;
        yyjson_val *k_val = yyjson_obj_get(sitem, "k");
        if (k_val != NULL && yyjson_is_int(k_val)) {
            int64_t kv = yyjson_get_int(k_val);
            if (kv >= 1 && kv <= 1000)
                k = (size_t) kv;
        }

        size_t offset = 0;
        yyjson_val *off_val = yyjson_obj_get(sitem, "offset");
        if (off_val != NULL && yyjson_is_int(off_val)) {
            int64_t ov = yyjson_get_int(off_val);
            if (ov >= 0 && ov <= 999)
                offset = (size_t) ov;
        }

        const av_opclass *oc = coldef->opclass;
        yyjson_val *op_val = yyjson_obj_get(sitem, "operator");
        if (op_val != NULL && yyjson_is_str(op_val)) {
            const av_opclass *found_oc = av_opclass_for_operator(coldef->vtype, yyjson_get_str(op_val));
            if (found_oc != NULL)
                oc = found_oc;
        }

        int inc_flags = AV_INCLUDE_ATTRIBUTES;
        yyjson_val *inc_val = yyjson_obj_get(sitem, "include");
        if (inc_val != NULL && yyjson_is_arr(inc_val)) {
            inc_flags = 0;
            size_t ii, imax;
            yyjson_val *ielem;
            yyjson_arr_foreach(inc_val, ii, imax, ielem)
            {
                const char *is = yyjson_get_str(ielem);
                if (is != NULL) {
                    if (strcmp(is, "attributes") == 0)
                        inc_flags |= AV_INCLUDE_ATTRIBUTES;
                    else if (strcmp(is, "document") == 0)
                        inc_flags |= AV_INCLUDE_DOCUMENT;
                    else if (strcmp(is, "vector") == 0)
                        inc_flags |= AV_INCLUDE_VECTOR;
                }
            }
        }

        bool exact = false;
        yyjson_val *ex_val = yyjson_obj_get(sitem, "exact");
        if (ex_val != NULL && yyjson_is_bool(ex_val))
            exact = yyjson_get_bool(ex_val);

        bool explain = false;
        yyjson_val *exp_val = yyjson_obj_get(sitem, "explain");
        if (exp_val != NULL && yyjson_is_bool(exp_val))
            explain = yyjson_get_bool(exp_val);

        av_filter *ast = NULL;
        uint64_t fhash = 0;
        (void) parse_filter_or_where(sitem, coldef, req->arena, &ast, &fhash, &e);

        roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, live, as_of, &e);

        av_search_params sparams;
        memset(&sparams, 0, sizeof(sparams));
        sparams.query_vec = query_vec;
        sparams.oc = oc;
        sparams.k = k;
        sparams.offset = offset;
        sparams.exact = exact;
        sparams.include_flags = inc_flags;
        sparams.explain = explain;

        av_search_result sres;
        memset(&sres, 0, sizeof(sres));
        if (av_execute_search(c, store, &sparams, cand, &sres, &e) == AV_OK) {
            yyjson_mut_val *item_resp = yyjson_mut_obj(resp_doc);
            yyjson_mut_val *res_arr = yyjson_mut_arr(resp_doc);

            for (size_t hi = 0; hi < sres.n_hits; hi++) {
                const av_search_hit *h = &sres.hits[hi];
                yyjson_mut_val *ho = yyjson_mut_obj(resp_doc);
                char khex[67];
                av_key_to_hex(h->key, khex);
                yyjson_mut_obj_add_strcpy(resp_doc, ho, "key", khex);
                if (h->has_id)
                    yyjson_mut_obj_add_strcpy(resp_doc, ho, "id", h->id);
                else
                    yyjson_mut_obj_add_null(resp_doc, ho, "id");
                yyjson_mut_obj_add_real(resp_doc, ho, "distance", h->distance);
                yyjson_mut_obj_add_real(resp_doc, ho, "score", h->score);
                char ahex[43];
                av_addr_to_hex(h->owner, ahex);
                yyjson_mut_obj_add_strcpy(resp_doc, ho, "owner", ahex);
                av_addr_to_hex(h->creator, ahex);
                yyjson_mut_obj_add_strcpy(resp_doc, ho, "creator", ahex);
                yyjson_mut_obj_add_uint(resp_doc, ho, "expires_at_block", h->expires_at);
                yyjson_mut_arr_append(res_arr, ho);
            }
            yyjson_mut_obj_add_val(resp_doc, item_resp, "results", res_arr);

            if (explain) {
                yyjson_mut_val *plan_obj = yyjson_mut_obj(resp_doc);
                yyjson_mut_obj_add_str(resp_doc, plan_obj, "strategy",
                                       av_plan_strategy_name(sres.plan.strategy));
                yyjson_mut_obj_add_bool(resp_doc, plan_obj, "exact", sres.plan.exact);
                yyjson_mut_obj_add_real(resp_doc, plan_obj, "elapsed_ms", sres.plan.elapsed_ms);
                yyjson_mut_obj_add_val(resp_doc, item_resp, "plan", plan_obj);
            }

            yyjson_mut_arr_append(resp_arr, item_resp);
            av_search_result_free(&sres);
        }

        if (cand != NULL)
            roaring_bitmap_free(cand);
    }

    av_collection_read_unlock(c);
    yyjson_doc_free(req_doc);

    yyjson_mut_obj_add_val(resp_doc, resp_root, "responses", resp_arr);

    resp->has_as_of = true;
    resp->as_of_block = as_of;
    (void) av_resp_json(resp, 200, resp_doc);
    av_collection_release(c);
}

/* Debug routes */
typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *arr;
    bool oom;
} debug_keys_acc;

static bool debug_keys_cb(const uint8_t key[32], uint64_t updated_at, uint64_t expires_at, void *ctx)
{
    debug_keys_acc *acc = (debug_keys_acc *) ctx;
    yyjson_mut_val *item = yyjson_mut_obj(acc->doc);
    char hex[67];
    av_key_to_hex(key, hex);
    yyjson_mut_obj_add_strcpy(acc->doc, item, "key", hex);
    yyjson_mut_obj_add_uint(acc->doc, item, "updated_at", updated_at);
    yyjson_mut_obj_add_uint(acc->doc, item, "expires_at", expires_at);
    if (!yyjson_mut_arr_append(acc->arr, item)) {
        acc->oom = true;
        return false;
    }
    return true;
}

static void h_debug_keys(const av_http_req *req, const char *const *params, size_t n_params,
                         av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *name = params[0];
    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_collection *c = av_registry_get(reg, name);
    if (c == NULL) {
        av_err e;
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    uint64_t as_of = av_collection_as_of(c);
    yyjson_mut_obj_add_uint(doc, root, "as_of_block", as_of);

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "keys", arr);

    debug_keys_acc acc = {doc, arr, false};
    av_collection_each_live(c, debug_keys_cb, &acc);

    if (acc.oom) {
        yyjson_mut_doc_free(doc);
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }

    resp->has_as_of = true;
    resp->as_of_block = as_of;
    (void) av_resp_json(resp, 200, doc);
    av_collection_release(c);
}

static void h_debug_entity(const av_http_req *req, const char *const *params, size_t n_params,
                           av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *coll_name = params[0];
    const char *key_str = params[1];

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_collection *c = av_registry_get(reg, coll_name);
    if (c == NULL) {
        av_err e;
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    uint8_t key[32];
    if (strlen(key_str) != 66 || strncmp(key_str, "0x", 2) != 0 || !av_key_from_hex(key_str, key)) {
        av_collection_release(c);
        av_err e;
        av_err_set(&e, "invalid_argument", "key must be 0x followed by 64 hex characters");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_store *store = av_rest_get_store();
    if (store == NULL) {
        av_collection_release(c);
        fail_internal(resp, "store service not available", req->path);
        return;
    }

    av_row row;
    bool found = false;
    av_err e;
    av_err_clear(&e);
    if (av_store_get_entity(store, coll_name, key, req->arena, &row, &found, &e) != AV_OK || !found) {
        av_collection_release(c);
        av_err_set(&e, "entity_not_found", "entity '%s' not found in store", key_str);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_attr *attrs = NULL;
    size_t n_attrs = 0;
    if (av_attrs_unpack(row.attrs_blob, row.attrs_len, req->arena, &attrs, &n_attrs, &e) != AV_OK) {
        av_collection_release(c);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "key", key_str);
    yyjson_mut_obj_add_uint(doc, root, "docid", row.docid);

    char addr[43];
    av_addr_to_hex(row.owner, addr);
    yyjson_mut_obj_add_strcpy(doc, root, "owner", addr);

    av_addr_to_hex(row.creator, addr);
    yyjson_mut_obj_add_strcpy(doc, root, "creator", addr);

    yyjson_mut_obj_add_uint(doc, root, "created_at", row.created_at);
    yyjson_mut_obj_add_uint(doc, root, "updated_at", row.updated_at);
    yyjson_mut_obj_add_uint(doc, root, "expires_at", row.expires_at);

    yyjson_mut_val *at_obj = yyjson_mut_obj(doc);
    for (size_t i = 0; i < n_attrs; i++) {
        yyjson_mut_val *tv = av_attr_to_typed_json(doc, &attrs[i], req->arena);
        /* yyjson does not copy keys added with obj_add_val: copy it, the source may be short-lived */
        {
            yyjson_mut_val *k_ = yyjson_mut_strcpy(doc, attrs[i].name);
            if (k_ != NULL && tv != NULL)
                yyjson_mut_obj_add(at_obj, k_, tv);
        }
    }
    yyjson_mut_obj_add_val(doc, root, "attributes", at_obj);

    resp->has_as_of = true;
    resp->as_of_block = av_collection_as_of(c);
    (void) av_resp_json(resp, 200, doc);
    av_collection_release(c);
}

void av_rest_register_search(av_router *r, bool debug_routes, void *ctx)
{
    (void) ctx;

    av_err e;
    av_err_clear(&e);

    (void) av_router_add(r, "POST", "/v1/collections/{name}/search", h_search, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/search:batch", h_search_batch, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/scroll", h_scroll, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/count", h_count, NULL, &e);

    if (debug_routes) {
        (void) av_router_add(r, "GET", "/_debug/collections/{name}/keys", h_debug_keys, NULL, &e);
        (void) av_router_add(r, "GET", "/_debug/collections/{name}/entities/{key}", h_debug_entity, NULL, &e);
    }
}
