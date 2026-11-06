/*
 * server/rest_entities.c: REST entity endpoints (point lookup, write stubs).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "arkiv/rpc.h"
#include "codec/base64.h"
#include "codec/hex.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/eval.h"
#include "filter/json_filter.h"
#include "filter/parser.h"
#include "filter/value.h"
#include "filter/value_json.h"
#include "query/executor.h"
#include "server/idempotency.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/rest.h"
#include "store/lmdb_store.h"
#include "store/schema.h"
#include "write/writer.h"
#include "yyjson.h"

static void fail_internal(av_http_resp *resp, const char *msg, const char *instance)
{
    av_err e;
    av_err_set(&e, "internal", "%s", msg);
    av_problem_from_err(&e, resp, instance);
}

/* Parse a decimal integer query value; returns def when malformed, clamps to [lo, hi]. */
static long parse_long_clamped(const char *s, long lo, long hi, long def)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0)
        return def;
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Parse a parameter value from a query string */
static bool get_query_param(const char *query, const char *key, char *out, size_t cap)
{
    if (query == NULL || query[0] == '\0' || key == NULL) {
        return false;
    }
    size_t klen = strlen(key);
    const char *p = query;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (amp == NULL) {
            amp = p + strlen(p);
        }
        if (eq != NULL && eq < amp && (size_t) (eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = (size_t) (amp - (eq + 1));
            if (vlen >= cap) {
                vlen = cap - 1;
            }
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = *amp == '&' ? amp + 1 : amp;
    }
    return false;
}

static void h_get_entity(const av_http_req *req, const char *const *params, size_t n_params,
                         av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;

    const char *coll_name = params[0];
    const char *ref = params[1];

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

    /* Query params */
    char buf[256];
    int include_flags = AV_INCLUDE_ATTRIBUTES;
    if (get_query_param(req->query, "include", buf, sizeof(buf))) {
        include_flags = 0;
        char *saveptr = NULL;
        char *token = strtok_r(buf, ",", &saveptr);
        while (token != NULL) {
            if (strcmp(token, "attributes") == 0) {
                include_flags |= AV_INCLUDE_ATTRIBUTES;
            } else if (strcmp(token, "document") == 0) {
                include_flags |= AV_INCLUDE_DOCUMENT;
            } else if (strcmp(token, "vector") == 0) {
                include_flags |= AV_INCLUDE_VECTOR;
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
    }

    bool do_verify = false;
    if (get_query_param(req->query, "verify", buf, sizeof(buf))) {
        if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
            do_verify = true;
        }
    }

    int timeout_ms = 5000;
    if (get_query_param(req->query, "timeout_ms", buf, sizeof(buf))) {
        timeout_ms = (int) parse_long_clamped(buf, 0, 60000, 5000);
        if (timeout_ms < 0)
            timeout_ms = 0;
        if (timeout_ms > 60000)
            timeout_ms = 60000;
    }

    if (get_query_param(req->query, "min_block", buf, sizeof(buf))) {
        uint64_t min_block = (uint64_t) strtoull(buf, NULL, 10);
        av_err e;
        av_err_clear(&e);
        if (av_collection_wait_block(c, min_block, timeout_ms, &e) != AV_OK) {
            av_collection_release(c);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    } else if (av_collection_is_bootstrapping(c)) {
        av_collection_release(c);
        av_err e;
        av_err_set(&e, "not_ready", "collection is bootstrapping; retry shortly");
        resp->retry_after = "1";
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Resolve ref to 32-byte key */
    uint8_t key[32];
    bool resolved = false;
    if (strlen(ref) == 66 && strncmp(ref, "0x", 2) == 0 && av_key_from_hex(ref, key)) {
        resolved = true;
    } else {
        /* External id lookup */
        av_store *store = av_rest_get_store();
        if (store != NULL) {
            bool found_id = false;
            av_err e;
            av_err_clear(&e);
            if (av_store_get_id(store, coll_name, ref, strlen(ref), key, &found_id, &e) == AV_OK &&
                found_id) {
                resolved = true;
            }
        }
    }

    if (!resolved) {
        av_collection_release(c);
        av_err e;
        av_err_set(&e, "entity_not_found", "entity '%s' not found in collection '%s'", ref, coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Check if entity is live */
    av_collection_read_lock(c);
    bool is_live = av_collection_has_key(c, key);
    uint64_t as_of = av_collection_as_of(c);
    av_collection_read_unlock(c);

    if (!is_live) {
        av_collection_release(c);
        av_err e;
        av_err_set(&e, "entity_not_found", "entity '%s' is not live in collection '%s'", ref, coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Fetch entity row from store */
    av_store *store = av_rest_get_store();
    if (store == NULL) {
        av_collection_release(c);
        fail_internal(resp, "store service not available", req->path);
        return;
    }

    av_row row;
    bool row_found = false;
    av_err e;
    av_err_clear(&e);
    if (av_store_get_entity(store, coll_name, key, req->arena, &row, &row_found, &e) != AV_OK || !row_found) {
        av_collection_release(c);
        av_err_set(&e, "entity_not_found", "entity row '%s' not found in store", ref);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    /* Unpack attributes */
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

    yyjson_mut_obj_add_uint(doc, root, "as_of_block", as_of);

    yyjson_mut_val *eo = yyjson_mut_obj(doc);
    char khex[67];
    av_key_to_hex(key, khex);
    yyjson_mut_obj_add_strcpy(doc, eo, "key", khex);

    /* Extract id, doc, vec, and user attributes */
    const char *id_val = NULL;
    size_t id_len = 0;
    const char *doc_val = NULL;
    size_t doc_len = 0;
    const uint8_t *vec_val = NULL;
    size_t vec_len = 0;

    yyjson_mut_val *attrs_obj = yyjson_mut_obj(doc);

    for (size_t i = 0; i < n_attrs; i++) {
        const av_attr *at = &attrs[i];
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
                yyjson_mut_obj_add_val(doc, attrs_obj, at->name, tv);
            }
        }
    }

    if (id_val != NULL) {
        yyjson_mut_obj_add_strncpy(doc, eo, "id", id_val, id_len);
    } else {
        yyjson_mut_obj_add_null(doc, eo, "id");
    }

    if (do_verify) {
        bool verified = false;
        av_rpc *rpc = av_rest_get_rpc();
        if (rpc != NULL) {
            av_entity onchain_ent;
            memset(&onchain_ent, 0, sizeof(onchain_ent));
            bool found = false;
            av_err rpc_err;
            av_err_clear(&rpc_err);
            if (av_rpc_get_entity(rpc, key, req->arena, &onchain_ent, &found, &rpc_err) == AV_OK && found) {
                if (memcmp(onchain_ent.key, key, 32) == 0 && memcmp(onchain_ent.owner, row.owner, 20) == 0 &&
                    onchain_ent.updated_at == row.updated_at) {
                    verified = true;
                }
            }
        }
        yyjson_mut_obj_add_bool(doc, root, "verified", verified);
    }

    char addr_hex[43];
    av_addr_to_hex(row.owner, addr_hex);
    yyjson_mut_obj_add_strcpy(doc, eo, "owner", addr_hex);

    av_addr_to_hex(row.creator, addr_hex);
    yyjson_mut_obj_add_strcpy(doc, eo, "creator", addr_hex);

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

    yyjson_mut_obj_add_val(doc, root, "entity", eo);

    resp->has_as_of = true;
    resp->as_of_block = as_of;
    (void) av_resp_json(resp, 200, doc);
    av_collection_release(c);
}

static bool check_idempotency(const av_http_req *req, av_http_resp *resp, av_idempotency_store **out_idem,
                              void **out_handle)
{
    *out_idem = av_rest_get_idempotency();
    *out_handle = NULL;
    if (req->idempotency_key == NULL || req->idempotency_key[0] == '\0' || *out_idem == NULL) {
        return true;
    }
    av_idem_action action = AV_IDEM_NEW;
    av_idem_cached_response cached;
    memset(&cached, 0, sizeof(cached));
    av_err e;
    av_err_clear(&e);

    if (av_idempotency_check(*out_idem, req->idempotency_key, req->method, req->path, req->body,
                             req->body_len, &action, &cached, out_handle, &e) != AV_OK) {
        av_problem_from_err(&e, resp, req->path);
        return false;
    }
    if (action == AV_IDEM_REPLAY) {
        resp->status = cached.status_code;
        if (cached.content_type != NULL) {
            resp->content_type = strdup(cached.content_type);
        }
        if (cached.body != NULL) {
            resp->body = strdup(cached.body);
            resp->body_len = cached.body_len;
        }
        av_idem_cached_response_free(&cached);
        return false;
    }
    if (action == AV_IDEM_CONFLICT) {
        av_err_set(&e, "idempotency_conflict", "Idempotency-Key reused with different request payload");
        av_problem_from_err(&e, resp, req->path);
        return false;
    }
    return true;
}

static void complete_idempotency(av_idempotency_store *idem, void *handle, av_http_resp *resp)
{
    if (idem != NULL && handle != NULL) {
        av_idempotency_complete(idem, handle, resp->status,
                                resp->content_type != NULL ? resp->content_type : "application/json",
                                resp->body, resp->body_len);
    }
}

static av_status parse_filter_or_where(yyjson_val *root, const av_coldef *coldef, av_arena *arena,
                                       av_filter **out_ast, av_err *e)
{
    *out_ast = NULL;
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
        return av_filter_from_where(w, wlen, coldef, arena, out_ast, e);
    }

    return AV_OK;
}

static void h_prepare_entities(const av_http_req *req, const char *const *params, size_t n_params,
                               av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    av_idempotency_store *idem = NULL;
    void *idem_handle = NULL;
    if (!check_idempotency(req, resp, &idem, &idem_handle)) {
        return;
    }

    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = yyjson_read_opts((char *) req->body, req->body_len, 0, NULL, NULL);
    if (req_doc == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "bad_request", "invalid JSON request body");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *from_val = yyjson_obj_get(root, "from");
    yyjson_val *ents_val = yyjson_obj_get(root, "entities");
    const char *from_str = yyjson_get_str(from_val);

    if (from_str == NULL || ents_val == NULL || !yyjson_is_arr(ents_val)) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "invalid_argument", "request must contain 'from' address and 'entities' array");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    uint8_t from_addr[20];
    if (!av_addr_from_hex(from_str, from_addr)) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "invalid_argument", "'from' must be a valid 20-byte hex address");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    av_collection *c = reg != NULL ? av_registry_get(reg, coll_name) : NULL;
    if (c == NULL) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_writer *w = av_rest_get_writer();
    bool temp_writer = false;
    if (w == NULL) {
        av_writer_config wcfg;
        memset(&wcfg, 0, sizeof(wcfg));
        w = av_writer_create(&wcfg, NULL, av_rest_get_rpc(), av_rest_get_store(), reg, &e);
        temp_writer = true;
    }

    av_prepare_result out;
    memset(&out, 0, sizeof(out));
    if (av_writer_prepare(w, c, ents_val, from_addr, req->arena, &out, &e) != AV_OK) {
        if (temp_writer)
            av_writer_destroy(w);
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *res_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res_root = yyjson_mut_obj(res_doc);
    yyjson_mut_doc_set_root(res_doc, res_root);
    yyjson_mut_obj_add_uint(res_doc, res_root, "chain_id", out.chain_id);

    yyjson_mut_val *txs_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_transactions; i++) {
        yyjson_mut_val *to = yyjson_mut_obj(res_doc);
        yyjson_mut_obj_add_strcpy(res_doc, to, "to", out.transactions[i].to);
        yyjson_mut_obj_add_strcpy(res_doc, to, "data", out.transactions[i].data);
        yyjson_mut_obj_add_strcpy(res_doc, to, "value", out.transactions[i].value);
        yyjson_mut_obj_add_uint(res_doc, to, "nonce", out.transactions[i].nonce);
        yyjson_mut_obj_add_uint(res_doc, to, "gas_estimate", out.transactions[i].gas_estimate);
        yyjson_mut_arr_add_val(txs_arr, to);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "transactions", txs_arr);

    yyjson_mut_val *res_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_results; i++) {
        yyjson_mut_val *ro = yyjson_mut_obj(res_doc);
        if (out.results[i].id[0] != '\0') {
            yyjson_mut_obj_add_strcpy(res_doc, ro, "id", out.results[i].id);
        } else {
            yyjson_mut_obj_add_null(res_doc, ro, "id");
        }
        char khex[67];
        av_key_to_hex(out.results[i].key, khex);
        yyjson_mut_obj_add_strcpy(res_doc, ro, "key", khex);
        yyjson_mut_obj_add_strcpy(res_doc, ro, "op", out.results[i].op);
        yyjson_mut_arr_add_val(res_arr, ro);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "results", res_arr);

    (void) av_resp_json(resp, 200, res_doc);
    complete_idempotency(idem, idem_handle, resp);

    if (temp_writer)
        av_writer_destroy(w);
    av_collection_release(c);
    yyjson_doc_free(req_doc);
}

static void h_upsert_entities(const av_http_req *req, const char *const *params, size_t n_params,
                              av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    av_idempotency_store *idem = NULL;
    void *idem_handle = NULL;
    if (!check_idempotency(req, resp, &idem, &idem_handle)) {
        return;
    }

    av_err e;
    av_err_clear(&e);

    av_writer *w = av_rest_get_writer();
    if (w == NULL || av_writer_get_signer(w) == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "write_disabled", "managed entity writes require a configured signer");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_doc *req_doc = yyjson_read_opts((char *) req->body, req->body_len, 0, NULL, NULL);
    if (req_doc == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "bad_request", "invalid JSON request body");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *ents_val = yyjson_obj_get(root, "entities");
    if (ents_val == NULL || !yyjson_is_arr(ents_val)) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "invalid_argument", "request must contain 'entities' array");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    char buf[64];
    av_write_wait_mode wait_mode = AV_WAIT_RECEIPT;
    if (get_query_param(req->query, "wait", buf, sizeof(buf))) {
        if (strcmp(buf, "none") == 0)
            wait_mode = AV_WAIT_NONE;
        else if (strcmp(buf, "receipt") == 0)
            wait_mode = AV_WAIT_RECEIPT;
        else if (strcmp(buf, "indexed") == 0)
            wait_mode = AV_WAIT_INDEXED;
    } else {
        yyjson_val *wval = yyjson_obj_get(root, "wait");
        if (wval != NULL && yyjson_is_str(wval)) {
            const char *ws = yyjson_get_str(wval);
            if (strcmp(ws, "none") == 0)
                wait_mode = AV_WAIT_NONE;
            else if (strcmp(ws, "receipt") == 0)
                wait_mode = AV_WAIT_RECEIPT;
            else if (strcmp(ws, "indexed") == 0)
                wait_mode = AV_WAIT_INDEXED;
        }
    }

    int timeout_ms = 60000;
    if (get_query_param(req->query, "timeout_ms", buf, sizeof(buf))) {
        timeout_ms = (int) parse_long_clamped(buf, 0, 600000, 60000);
    } else {
        yyjson_val *tval = yyjson_obj_get(root, "timeout_ms");
        if (tval != NULL && yyjson_is_num(tval)) {
            timeout_ms = (int) yyjson_get_num(tval);
        }
    }
    if (timeout_ms <= 0)
        timeout_ms = 60000;

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    av_collection *c = reg != NULL ? av_registry_get(reg, coll_name) : NULL;
    if (c == NULL) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_upsert_batch_result out;
    memset(&out, 0, sizeof(out));
    if (av_writer_upsert(w, c, ents_val, wait_mode, timeout_ms, req->arena, &out, &e) != AV_OK) {
        av_collection_release(c);
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *res_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res_root = yyjson_mut_obj(res_doc);
    yyjson_mut_doc_set_root(res_doc, res_root);

    yyjson_mut_val *res_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_results; i++) {
        yyjson_mut_val *ro = yyjson_mut_obj(res_doc);
        if (out.results[i].id[0] != '\0') {
            yyjson_mut_obj_add_strcpy(res_doc, ro, "id", out.results[i].id);
        } else {
            yyjson_mut_obj_add_null(res_doc, ro, "id");
        }
        char khex[67];
        av_key_to_hex(out.results[i].key, khex);
        yyjson_mut_obj_add_strcpy(res_doc, ro, "key", khex);
        yyjson_mut_obj_add_strcpy(res_doc, ro, "op", out.results[i].op);
        yyjson_mut_arr_add_val(res_arr, ro);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "results", res_arr);

    yyjson_mut_val *txs_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_tx_hashes; i++) {
        yyjson_mut_arr_add_strcpy(res_doc, txs_arr, out.tx_hashes[i]);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "tx_hashes", txs_arr);

    if (wait_mode == AV_WAIT_NONE || !out.has_block_number) {
        yyjson_mut_obj_add_null(res_doc, res_root, "block_number");
    } else {
        yyjson_mut_obj_add_uint(res_doc, res_root, "block_number", out.block_number);
    }

    int http_status = (wait_mode == AV_WAIT_NONE ? 202 : 200);
    (void) av_resp_json(resp, http_status, res_doc);
    complete_idempotency(idem, idem_handle, resp);

    av_collection_release(c);
    yyjson_doc_free(req_doc);
}

static void h_delete_entity(const av_http_req *req, const char *const *params, size_t n_params,
                            av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    av_idempotency_store *idem = NULL;
    void *idem_handle = NULL;
    if (!check_idempotency(req, resp, &idem, &idem_handle)) {
        return;
    }

    av_err e;
    av_err_clear(&e);

    av_writer *w = av_rest_get_writer();
    if (w == NULL || av_writer_get_signer(w) == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "write_disabled", "managed entity delete requires a configured signer");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const char *coll_name = params[0];
    const char *ref = params[1];

    av_registry *reg = av_rest_get_registry();
    av_collection *c = reg != NULL ? av_registry_get(reg, coll_name) : NULL;
    if (c == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const char *refs[1] = {ref};
    av_tx_response out;
    memset(&out, 0, sizeof(out));

    if (av_writer_delete_refs(w, c, refs, 1, req->arena, &out, &e) != AV_OK) {
        av_collection_release(c);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *res_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res_root = yyjson_mut_obj(res_doc);
    yyjson_mut_doc_set_root(res_doc, res_root);

    yyjson_mut_obj_add_uint(res_doc, res_root, "affected", out.affected);
    yyjson_mut_val *txs_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_tx_hashes; i++) {
        yyjson_mut_arr_add_strcpy(res_doc, txs_arr, out.tx_hashes[i]);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "tx_hashes", txs_arr);
    yyjson_mut_obj_add_uint(res_doc, res_root, "block_number", out.block_number);

    (void) av_resp_json(resp, 200, res_doc);
    complete_idempotency(idem, idem_handle, resp);
    av_collection_release(c);
}

static void h_delete_entities(const av_http_req *req, const char *const *params, size_t n_params,
                              av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    av_idempotency_store *idem = NULL;
    void *idem_handle = NULL;
    if (!check_idempotency(req, resp, &idem, &idem_handle)) {
        return;
    }

    av_err e;
    av_err_clear(&e);

    av_writer *w = av_rest_get_writer();
    if (w == NULL || av_writer_get_signer(w) == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "write_disabled", "managed entity delete requires a configured signer");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_doc *req_doc = yyjson_read_opts((char *) req->body, req->body_len, 0, NULL, NULL);
    if (req_doc == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "bad_request", "invalid JSON request body");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *refs_val = yyjson_obj_get(root, "refs");
    yyjson_val *filt_val = yyjson_obj_get(root, "filter");
    yyjson_val *where_val = yyjson_obj_get(root, "where");

    int selectors_set = (refs_val != NULL) + (filt_val != NULL) + (where_val != NULL);
    if (selectors_set != 1) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "invalid_argument", "must specify exactly one of 'refs', 'filter', or 'where'");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    av_collection *c = reg != NULL ? av_registry_get(reg, coll_name) : NULL;
    if (c == NULL) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_tx_response out;
    memset(&out, 0, sizeof(out));

    if (refs_val != NULL) {
        if (!yyjson_is_arr(refs_val)) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_err_set(&e, "invalid_argument", "'refs' must be an array");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        size_t n_refs = yyjson_arr_size(refs_val);
        if (n_refs > 1000) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_err_set(&e, "invalid_argument", "refs exceeds limit of 1000 entities");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        const char **refs = av_arena_alloc(req->arena, n_refs * sizeof(char *), 0);
        for (size_t i = 0; i < n_refs; i++) {
            refs[i] = yyjson_get_str(yyjson_arr_get(refs_val, i));
            if (refs[i] == NULL)
                refs[i] = "";
        }
        if (av_writer_delete_refs(w, c, refs, n_refs, req->arena, &out, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    } else {
        const av_coldef *coldef = av_collection_def(c);
        av_filter *ast = NULL;
        if (parse_filter_or_where(root, coldef, req->arena, &ast, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        av_collection_read_lock(c);
        uint64_t as_of = av_collection_as_of(c);
        const roaring_bitmap_t *live = av_collection_live(c);
        const av_docmap *docmap = av_collection_docmap(c);
        const av_attr_index *ix = av_collection_attr_index(c);

        roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, live, as_of, &e);
        av_collection_read_unlock(c);

        if (cand == NULL && ast != NULL && ast->kind != AV_F_TRUE) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        uint64_t count = (cand != NULL) ? roaring_bitmap_get_cardinality(cand) : 0;
        if (count > 10000) {
            if (cand != NULL)
                roaring_bitmap_free(cand);
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_err_set(&e, "invalid_argument", "filter matches %lu entities, exceeding 10000 cap",
                       (unsigned long) count);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        if (av_writer_delete_filter(w, c, cand, req->arena, &out, &e) != AV_OK) {
            if (cand != NULL)
                roaring_bitmap_free(cand);
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        if (cand != NULL)
            roaring_bitmap_free(cand);
    }

    yyjson_mut_doc *res_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res_root = yyjson_mut_obj(res_doc);
    yyjson_mut_doc_set_root(res_doc, res_root);

    yyjson_mut_obj_add_uint(res_doc, res_root, "affected", out.affected);
    yyjson_mut_val *txs_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_tx_hashes; i++) {
        yyjson_mut_arr_add_strcpy(res_doc, txs_arr, out.tx_hashes[i]);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "tx_hashes", txs_arr);
    yyjson_mut_obj_add_uint(res_doc, res_root, "block_number", out.block_number);

    (void) av_resp_json(resp, 200, res_doc);
    complete_idempotency(idem, idem_handle, resp);
    av_collection_release(c);
    yyjson_doc_free(req_doc);
}

static void h_extend_entities(const av_http_req *req, const char *const *params, size_t n_params,
                              av_http_resp *resp, void *ctx)
{
    (void) n_params;
    (void) ctx;
    av_idempotency_store *idem = NULL;
    void *idem_handle = NULL;
    if (!check_idempotency(req, resp, &idem, &idem_handle)) {
        return;
    }

    av_err e;
    av_err_clear(&e);

    av_writer *w = av_rest_get_writer();
    if (w == NULL || av_writer_get_signer(w) == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "write_disabled", "managed entity extend requires a configured signer");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_doc *req_doc = yyjson_read_opts((char *) req->body, req->body_len, 0, NULL, NULL);
    if (req_doc == NULL) {
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "bad_request", "invalid JSON request body");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *ttl_val = yyjson_obj_get(root, "ttl");
    if (ttl_val == NULL) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "invalid_argument", "extend request requires 'ttl' object");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    uint64_t expires_at = 0, min_lifetime = 0;
    if (!av_writer_parse_ttl(ttl_val, &expires_at, &min_lifetime, &e)) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *refs_val = yyjson_obj_get(root, "refs");
    yyjson_val *filt_val = yyjson_obj_get(root, "filter");
    yyjson_val *where_val = yyjson_obj_get(root, "where");

    int selectors_set = (refs_val != NULL) + (filt_val != NULL) + (where_val != NULL);
    if (selectors_set != 1) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "invalid_argument", "must specify exactly one of 'refs', 'filter', or 'where'");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    const char *coll_name = params[0];
    av_registry *reg = av_rest_get_registry();
    av_collection *c = reg != NULL ? av_registry_get(reg, coll_name) : NULL;
    if (c == NULL) {
        yyjson_doc_free(req_doc);
        if (idem_handle != NULL)
            av_idempotency_abort(idem, idem_handle);
        av_err_set(&e, "collection_not_found", "collection '%s' does not exist", coll_name);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_tx_response out;
    memset(&out, 0, sizeof(out));

    if (refs_val != NULL) {
        if (!yyjson_is_arr(refs_val)) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_err_set(&e, "invalid_argument", "'refs' must be an array");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        size_t n_refs = yyjson_arr_size(refs_val);
        if (n_refs > 1000) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_err_set(&e, "invalid_argument", "refs exceeds limit of 1000 entities");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        const char **refs = av_arena_alloc(req->arena, n_refs * sizeof(char *), 0);
        for (size_t i = 0; i < n_refs; i++) {
            refs[i] = yyjson_get_str(yyjson_arr_get(refs_val, i));
            if (refs[i] == NULL)
                refs[i] = "";
        }
        if (av_writer_extend_refs(w, c, refs, n_refs, expires_at, min_lifetime, req->arena, &out, &e) !=
            AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    } else {
        const av_coldef *coldef = av_collection_def(c);
        av_filter *ast = NULL;
        if (parse_filter_or_where(root, coldef, req->arena, &ast, &e) != AV_OK) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        av_collection_read_lock(c);
        uint64_t as_of = av_collection_as_of(c);
        const roaring_bitmap_t *live = av_collection_live(c);
        const av_docmap *docmap = av_collection_docmap(c);
        const av_attr_index *ix = av_collection_attr_index(c);

        roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, live, as_of, &e);
        av_collection_read_unlock(c);

        if (cand == NULL && ast != NULL && ast->kind != AV_F_TRUE) {
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        uint64_t count = (cand != NULL) ? roaring_bitmap_get_cardinality(cand) : 0;
        if (count > 10000) {
            if (cand != NULL)
                roaring_bitmap_free(cand);
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_err_set(&e, "invalid_argument", "filter matches %lu entities, exceeding 10000 cap",
                       (unsigned long) count);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        if (av_writer_extend_filter(w, c, cand, expires_at, min_lifetime, req->arena, &out, &e) != AV_OK) {
            if (cand != NULL)
                roaring_bitmap_free(cand);
            av_collection_release(c);
            yyjson_doc_free(req_doc);
            if (idem_handle != NULL)
                av_idempotency_abort(idem, idem_handle);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        if (cand != NULL)
            roaring_bitmap_free(cand);
    }

    yyjson_mut_doc *res_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res_root = yyjson_mut_obj(res_doc);
    yyjson_mut_doc_set_root(res_doc, res_root);

    yyjson_mut_obj_add_uint(res_doc, res_root, "affected", out.affected);
    yyjson_mut_val *txs_arr = yyjson_mut_arr(res_doc);
    for (size_t i = 0; i < out.n_tx_hashes; i++) {
        yyjson_mut_arr_add_strcpy(res_doc, txs_arr, out.tx_hashes[i]);
    }
    yyjson_mut_obj_add_val(res_doc, res_root, "tx_hashes", txs_arr);
    yyjson_mut_obj_add_uint(res_doc, res_root, "block_number", out.block_number);

    (void) av_resp_json(resp, 200, res_doc);
    complete_idempotency(idem, idem_handle, resp);
    av_collection_release(c);
    yyjson_doc_free(req_doc);
}

void av_rest_register_entities(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;

    av_err e;
    av_err_clear(&e);

    /* Read route */
    (void) av_router_add(r, "GET", "/v1/collections/{name}/entities/{ref}", h_get_entity, NULL, &e);

    /* Write routes */
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities", h_upsert_entities, NULL, &e);
    (void) av_router_add(r, "DELETE", "/v1/collections/{name}/entities/{ref}", h_delete_entity, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities:delete", h_delete_entities, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities:extend", h_extend_entities, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities:prepare", h_prepare_entities, NULL, &e);
}
