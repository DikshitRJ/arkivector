/*
 * server/mcp.c: Model Context Protocol (MCP) Streamable HTTP endpoint (POST /mcp).
 *
 * Implements docs/api/mcp.md (protocol version 2025-06-18) enabling AI agents
 * to use Arkivector as an autonomous tool server.
 */
#include "server/mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/hex.h"
#include "codec/text.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/eval.h"
#include "filter/parser.h"
#include "index/docid.h"
#include "index/live.h"
#include "kernels/opclass.h"
#include "query/executor.h"
#include "query/planner.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/rest.h"
#include "store/lmdb_store.h"
#include "store/schema.h"
#include "version.h"
#include "write/writer.h"
#include "yyjson.h"

static void mcp_resp_json(av_http_resp *resp, yyjson_mut_val *id_val, yyjson_mut_val *result_val,
                          yyjson_mut_val *error_val)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    if (id_val != NULL) {
        yyjson_mut_obj_add_val(doc, root, "id", yyjson_mut_val_mut_copy(doc, id_val));
    } else {
        yyjson_mut_obj_add_null(doc, root, "id");
    }

    if (error_val != NULL) {
        yyjson_mut_obj_add_val(doc, root, "error", yyjson_mut_val_mut_copy(doc, error_val));
    } else if (result_val != NULL) {
        yyjson_mut_obj_add_val(doc, root, "result", yyjson_mut_val_mut_copy(doc, result_val));
    } else {
        yyjson_mut_obj_add_null(doc, root, "result");
    }

    resp->status = 200;
    resp->content_type = "application/json";
    size_t len = 0;
    resp->body = yyjson_mut_write(doc, 0, &len);
    resp->body_len = len;
    yyjson_mut_doc_free(doc);
}

static void mcp_tool_result(av_http_resp *resp, yyjson_mut_val *id_val, const char *text, bool is_error)
{
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(tmp);
    yyjson_mut_val *content = yyjson_mut_arr(tmp);
    yyjson_mut_val *item = yyjson_mut_obj(tmp);
    yyjson_mut_obj_add_strcpy(tmp, item, "type", "text");
    yyjson_mut_obj_add_strcpy(tmp, item, "text", text != NULL ? text : "");
    yyjson_mut_arr_add_val(content, item);
    yyjson_mut_obj_add_val(tmp, res, "content", content);
    yyjson_mut_obj_add_bool(tmp, res, "isError", is_error);

    mcp_resp_json(resp, id_val, res, NULL);
    yyjson_mut_doc_free(tmp);
}

static void handle_initialize(av_http_resp *resp, yyjson_mut_val *id_val)
{
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(tmp);
    yyjson_mut_obj_add_strcpy(tmp, res, "protocolVersion", "2025-06-18");

    yyjson_mut_val *caps = yyjson_mut_obj(tmp);
    yyjson_mut_val *tools = yyjson_mut_obj(tmp);
    yyjson_mut_obj_add_val(tmp, caps, "tools", tools);
    yyjson_mut_obj_add_val(tmp, res, "capabilities", caps);

    yyjson_mut_val *srv = yyjson_mut_obj(tmp);
    yyjson_mut_obj_add_strcpy(tmp, srv, "name", "arkivector");
    yyjson_mut_obj_add_strcpy(tmp, srv, "version", AV_DAEMON_VERSION);
    yyjson_mut_obj_add_val(tmp, res, "serverInfo", srv);

    mcp_resp_json(resp, id_val, res, NULL);
    yyjson_mut_doc_free(tmp);
}

static void handle_tools_list(av_http_resp *resp, yyjson_mut_val *id_val)
{
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *res = yyjson_mut_obj(tmp);
    yyjson_mut_val *tools = yyjson_mut_arr(tmp);

    /* 1. arkivector_list_collections */
    {
        yyjson_mut_val *t = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, t, "name", "arkivector_list_collections");
        yyjson_mut_obj_add_strcpy(tmp, t, "description", "List available vector collections.");
        yyjson_mut_val *schema = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, schema, "type", "object");
        yyjson_mut_val *props = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_val(tmp, schema, "properties", props);
        yyjson_mut_obj_add_val(tmp, t, "inputSchema", schema);
        yyjson_mut_arr_add_val(tools, t);
    }
    /* 2. arkivector_search */
    {
        yyjson_mut_val *t = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, t, "name", "arkivector_search");
        yyjson_mut_obj_add_strcpy(tmp, t, "description",
                                  "Execute vector search with optional metadata filtering.");
        yyjson_mut_val *schema = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, schema, "type", "object");
        yyjson_mut_val *props = yyjson_mut_obj(tmp);
        yyjson_mut_val *c_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, c_prop, "type", "string");
        yyjson_mut_obj_add_val(tmp, props, "collection", c_prop);
        yyjson_mut_val *v_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, v_prop, "type", "array");
        yyjson_mut_obj_add_val(tmp, props, "vector", v_prop);
        yyjson_mut_val *k_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, k_prop, "type", "integer");
        yyjson_mut_obj_add_val(tmp, props, "k", k_prop);
        yyjson_mut_val *w_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, w_prop, "type", "string");
        yyjson_mut_obj_add_val(tmp, props, "where", w_prop);
        yyjson_mut_obj_add_val(tmp, schema, "properties", props);
        yyjson_mut_val *req = yyjson_mut_arr(tmp);
        yyjson_mut_arr_add_strcpy(tmp, req, "collection");
        yyjson_mut_obj_add_val(tmp, schema, "required", req);
        yyjson_mut_obj_add_val(tmp, t, "inputSchema", schema);
        yyjson_mut_arr_add_val(tools, t);
    }
    /* 3. arkivector_get */
    {
        yyjson_mut_val *t = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, t, "name", "arkivector_get");
        yyjson_mut_obj_add_strcpy(tmp, t, "description", "Fetch an entity by ID or key.");
        yyjson_mut_val *schema = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, schema, "type", "object");
        yyjson_mut_val *props = yyjson_mut_obj(tmp);
        yyjson_mut_val *c_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, c_prop, "type", "string");
        yyjson_mut_obj_add_val(tmp, props, "collection", c_prop);
        yyjson_mut_val *r_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, r_prop, "type", "string");
        yyjson_mut_obj_add_val(tmp, props, "ref", r_prop);
        yyjson_mut_obj_add_val(tmp, schema, "properties", props);
        yyjson_mut_val *req = yyjson_mut_arr(tmp);
        yyjson_mut_arr_add_strcpy(tmp, req, "collection");
        yyjson_mut_arr_add_strcpy(tmp, req, "ref");
        yyjson_mut_obj_add_val(tmp, schema, "required", req);
        yyjson_mut_obj_add_val(tmp, t, "inputSchema", schema);
        yyjson_mut_arr_add_val(tools, t);
    }
    /* 4. arkivector_count */
    {
        yyjson_mut_val *t = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, t, "name", "arkivector_count");
        yyjson_mut_obj_add_strcpy(tmp, t, "description", "Count entities matching an optional filter.");
        yyjson_mut_val *schema = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, schema, "type", "object");
        yyjson_mut_val *props = yyjson_mut_obj(tmp);
        yyjson_mut_val *c_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, c_prop, "type", "string");
        yyjson_mut_obj_add_val(tmp, props, "collection", c_prop);
        yyjson_mut_val *w_prop = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_strcpy(tmp, w_prop, "type", "string");
        yyjson_mut_obj_add_val(tmp, props, "where", w_prop);
        yyjson_mut_obj_add_val(tmp, schema, "properties", props);
        yyjson_mut_val *req = yyjson_mut_arr(tmp);
        yyjson_mut_arr_add_strcpy(tmp, req, "collection");
        yyjson_mut_obj_add_val(tmp, schema, "required", req);
        yyjson_mut_obj_add_val(tmp, t, "inputSchema", schema);
        yyjson_mut_arr_add_val(tools, t);
    }

    yyjson_mut_obj_add_val(tmp, res, "tools", tools);
    mcp_resp_json(resp, id_val, res, NULL);
    yyjson_mut_doc_free(tmp);
}

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *arr;
} mcp_list_acc;

static bool mcp_list_cb(av_collection *c, void *ctx)
{
    mcp_list_acc *acc = (mcp_list_acc *) ctx;
    const av_coldef *d = av_collection_def(c);
    yyjson_mut_val *o = yyjson_mut_obj(acc->doc);
    yyjson_mut_obj_add_strcpy(acc->doc, o, "name", d->name);
    yyjson_mut_obj_add_uint(acc->doc, o, "dimensions", (uint64_t) d->dimensions);
    yyjson_mut_arr_add_val(acc->arr, o);
    return true;
}

static void handle_tools_call(const av_http_req *req, av_http_resp *resp, yyjson_mut_val *id_val,
                              yyjson_val *params_val)
{
    if (params_val == NULL || !yyjson_is_obj(params_val)) {
        mcp_tool_result(resp, id_val, "invalid params: object expected", true);
        return;
    }

    const char *name = yyjson_get_str(yyjson_obj_get(params_val, "name"));
    yyjson_val *args = yyjson_obj_get(params_val, "arguments");
    if (name == NULL) {
        mcp_tool_result(resp, id_val, "missing tool name", true);
        return;
    }

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        mcp_tool_result(resp, id_val, "registry unavailable", true);
        return;
    }

    if (strcmp(name, "arkivector_list_collections") == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        mcp_list_acc acc = {doc, arr};
        av_registry_each(reg, mcp_list_cb, &acc);
        yyjson_mut_obj_add_val(doc, root, "collections", arr);

        char *json_text = yyjson_mut_write(doc, 0, NULL);
        mcp_tool_result(resp, id_val, json_text, false);
        free(json_text);
        yyjson_mut_doc_free(doc);
        return;
    }

    if (strcmp(name, "arkivector_count") == 0) {
        const char *coll = yyjson_get_str(yyjson_obj_get(args, "collection"));
        if (coll == NULL) {
            mcp_tool_result(resp, id_val, "missing 'collection' argument", true);
            return;
        }
        av_collection *c = av_registry_get(reg, coll);
        if (c == NULL) {
            mcp_tool_result(resp, id_val, "collection not found", true);
            return;
        }
        uint64_t cnt = (uint64_t) av_collection_live_count(c);
        uint64_t as_of = av_collection_as_of(c);

        const char *where_str = yyjson_get_str(yyjson_obj_get(args, "where"));
        if (where_str != NULL && where_str[0] != '\0') {
            const av_coldef *coldef = av_collection_def(c);
            av_filter *ast = NULL;
            av_err e;
            av_err_clear(&e);
            if (av_filter_from_where(where_str, strlen(where_str), coldef, req->arena, &ast, &e) == AV_OK) {
                av_collection_read_lock(c);
                const roaring_bitmap_t *clive = av_collection_live(c);
                const av_docmap *docmap = av_collection_docmap(c);
                const av_attr_index *ix = av_collection_attr_index(c);
                roaring_bitmap_t *cand = av_filter_eval(ast, ix, docmap, clive, as_of, &e);
                if (cand != NULL) {
                    cnt = roaring_bitmap_get_cardinality(cand);
                    roaring_bitmap_free(cand);
                }
                av_collection_read_unlock(c);
            }
        }
        av_collection_release(c);

        char out[128];
        snprintf(out, sizeof(out), "{\"count\": %llu, \"as_of_block\": %llu}", (unsigned long long) cnt,
                 (unsigned long long) as_of);
        mcp_tool_result(resp, id_val, out, false);
        return;
    }

    if (strcmp(name, "arkivector_get") == 0) {
        const char *coll = yyjson_get_str(yyjson_obj_get(args, "collection"));
        const char *ref = yyjson_get_str(yyjson_obj_get(args, "ref"));
        if (coll == NULL || ref == NULL) {
            mcp_tool_result(resp, id_val, "arguments 'collection' and 'ref' required", true);
            return;
        }
        av_collection *c = av_registry_get(reg, coll);
        if (c == NULL) {
            mcp_tool_result(resp, id_val, "collection not found", true);
            return;
        }
        uint64_t as_of = av_collection_as_of(c);
        av_store *store = av_rest_get_store();
        if (store == NULL) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "store unavailable", true);
            return;
        }
        uint8_t key[32];
        bool resolved = false;
        if (strlen(ref) == 66 && strncmp(ref, "0x", 2) == 0 && av_key_from_hex(ref, key)) {
            resolved = true;
        } else {
            bool found_id = false;
            av_err e;
            av_err_clear(&e);
            if (av_store_get_id(store, coll, ref, strlen(ref), key, &found_id, &e) == AV_OK && found_id) {
                resolved = true;
            }
        }
        if (!resolved) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "entity not found", true);
            return;
        }
        av_row row;
        bool found = false;
        av_err e;
        av_err_clear(&e);
        if (av_store_get_entity(store, coll, key, req->arena, &row, &found, &e) != AV_OK || !found) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "entity not found", true);
            return;
        }
        av_collection_release(c);

        char khex[67];
        av_key_to_hex(key, khex);
        char out[512];
        snprintf(out, sizeof(out),
                 "{\"as_of_block\": %llu, \"entity\": {\"key\": \"%s\", \"updated_at_block\": %llu}}",
                 (unsigned long long) as_of, khex, (unsigned long long) row.updated_at);
        mcp_tool_result(resp, id_val, out, false);
        return;
    }

    if (strcmp(name, "arkivector_search") == 0) {
        const char *coll = yyjson_get_str(yyjson_obj_get(args, "collection"));
        if (coll == NULL) {
            mcp_tool_result(resp, id_val, "argument 'collection' required", true);
            return;
        }
        av_collection *c = av_registry_get(reg, coll);
        if (c == NULL) {
            mcp_tool_result(resp, id_val, "collection not found", true);
            return;
        }
        const av_coldef *coldef = av_collection_def(c);
        yyjson_val *vec_val = yyjson_obj_get(args, "vector");
        if (vec_val == NULL || !yyjson_is_arr(vec_val)) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "missing or invalid 'vector' array", true);
            return;
        }
        size_t dim = yyjson_arr_size(vec_val);
        if ((int64_t) dim != coldef->dimensions) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "vector dimension mismatch", true);
            return;
        }
        size_t cap = dim * 32 + 16;
        char *buf = av_arena_alloc(req->arena, cap, 1);
        if (buf == NULL) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "out of memory", true);
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

        av_err err;
        av_err_clear(&err);
        void *query_pgvec = NULL;
        if (av_text_to_pgvec(coldef->vtype, buf, (int32_t) coldef->dimensions, req->arena, &query_pgvec,
                             &err) != AV_OK) {
            av_collection_release(c);
            mcp_tool_result(resp, id_val, "invalid vector elements", true);
            return;
        }

        size_t k = 10;
        yyjson_val *kv = yyjson_obj_get(args, "k");
        if (kv != NULL && yyjson_is_int(kv)) {
            int ki = yyjson_get_int(kv);
            if (ki > 0 && ki <= 50)
                k = (size_t) ki;
        }

        av_search_params sp;
        memset(&sp, 0, sizeof(sp));
        sp.query_vec = query_pgvec;
        sp.oc = coldef->opclass;
        sp.k = k;

        av_search_result sres;
        memset(&sres, 0, sizeof(sres));
        av_store *store = av_rest_get_store();
        av_collection_read_lock(c);
        av_status st = av_execute_search(c, store, &sp, NULL, &sres, &err);
        uint64_t as_of = av_collection_as_of(c);
        av_collection_read_unlock(c);
        av_collection_release(c);

        if (st != AV_OK) {
            mcp_tool_result(resp, id_val, err.msg[0] ? err.msg : "search failed", true);
            return;
        }

        yyjson_mut_doc *res_doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *res_obj = yyjson_mut_obj(res_doc);
        yyjson_mut_doc_set_root(res_doc, res_obj);
        yyjson_mut_obj_add_uint(res_doc, res_obj, "as_of_block", as_of);
        yyjson_mut_val *hits_arr = yyjson_mut_arr(res_doc);
        for (size_t i = 0; i < sres.n_hits; i++) {
            yyjson_mut_val *h = yyjson_mut_obj(res_doc);
            char khex[67];
            av_key_to_hex(sres.hits[i].key, khex);
            yyjson_mut_obj_add_strcpy(res_doc, h, "key", khex);
            if (sres.hits[i].has_id) {
                yyjson_mut_obj_add_strcpy(res_doc, h, "id", sres.hits[i].id);
            }
            yyjson_mut_obj_add_real(res_doc, h, "score", sres.hits[i].score);
            yyjson_mut_arr_add_val(hits_arr, h);
        }
        yyjson_mut_obj_add_val(res_doc, res_obj, "hits", hits_arr);
        char *json_text = yyjson_mut_write(res_doc, 0, NULL);
        mcp_tool_result(resp, id_val, json_text, false);
        free(json_text);
        yyjson_mut_doc_free(res_doc);
        av_search_result_free(&sres);
        return;
    }

    mcp_tool_result(resp, id_val, "unknown tool name", true);
}

/* POST /mcp */
static void h_mcp_post(const av_http_req *req, const char *const *params, size_t n_params, av_http_resp *resp,
                       void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;

    av_err e;
    av_err_clear(&e);
    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        resp->status = 400;
        resp->content_type = "application/json";
        resp->body = strdup(
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"},\"id\":null}");
        resp->body_len = strlen(resp->body);
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(req_doc);
    yyjson_val *method_val = yyjson_obj_get(root, "method");
    yyjson_val *id_val_src = yyjson_obj_get(root, "id");
    const char *method = yyjson_get_str(method_val);

    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *id_val = id_val_src != NULL ? yyjson_val_mut_copy(tmp, id_val_src) : NULL;

    if (method == NULL) {
        yyjson_mut_val *err = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_int(tmp, err, "code", -32600);
        yyjson_mut_obj_add_strcpy(tmp, err, "message", "Invalid Request");
        mcp_resp_json(resp, id_val, NULL, err);
        yyjson_mut_doc_free(tmp);
        yyjson_doc_free(req_doc);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(resp, id_val);
    } else if (strcmp(method, "notifications/initialized") == 0) {
        resp->status = 204;
        resp->body = NULL;
        resp->body_len = 0;
    } else if (strcmp(method, "tools/list") == 0) {
        handle_tools_list(resp, id_val);
    } else if (strcmp(method, "tools/call") == 0) {
        yyjson_val *params_val = yyjson_obj_get(root, "params");
        handle_tools_call(req, resp, id_val, params_val);
    } else {
        yyjson_mut_val *err = yyjson_mut_obj(tmp);
        yyjson_mut_obj_add_int(tmp, err, "code", -32601);
        yyjson_mut_obj_add_strcpy(tmp, err, "message", "Method not found");
        mcp_resp_json(resp, id_val, NULL, err);
    }

    yyjson_mut_doc_free(tmp);
    yyjson_doc_free(req_doc);
}

void av_rest_register_mcp(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;
    av_err e;
    av_err_clear(&e);
    (void) av_router_add(r, "POST", "/mcp", h_mcp_post, NULL, &e);
}
