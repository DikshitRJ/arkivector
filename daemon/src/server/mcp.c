/*
 * server/mcp.c: Model Context Protocol (MCP) server over Streamable HTTP (POST /mcp).
 *
 * Implements docs/api/mcp.md. JSON-RPC 2.0 messages are POSTed to /mcp and answered with a single
 * application/json response (no server-initiated SSE stream: GET and DELETE /mcp get 405 from the
 * router, which the MCP transport allows). The server is stateless: no Mcp-Session-Id.
 *
 * Every tool runs the corresponding /v1 handler through the router, so tool semantics are exactly
 * those of the REST API (docs/api/mcp.md), including validation and error codes. Each inner call
 * is checked against the caller's API key with the scope of its own /v1 route, so a read-only key
 * can search through MCP but not write.
 *
 * Security: requests carrying an Origin header from a non-loopback origin are rejected with 403
 * unless API-key auth is enabled (MCP transport spec: servers MUST validate Origin to prevent DNS
 * rebinding against local servers).
 */
#include "server/mcp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server/auth.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/rest.h"
#include "server/router.h"
#include "version.h"
#include "yyjson.h"

#define MCP_LATEST_VERSION "2025-06-18"
#define MCP_MAX_SEARCH_K   50
#define MCP_MAX_UPSERT     100

static const char *const k_versions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

/* Set by av_rest_register_mcp; the router is complete before the server starts and read-only after. */
static const av_router *g_router;

/* ------------------------------------------------------------------ JSON-RPC envelope */

static void rpc_write(av_http_resp *resp, yyjson_mut_doc *doc)
{
    size_t len = 0;
    resp->status = 200;
    resp->content_type = "application/json";
    resp->body = yyjson_mut_write(doc, 0, &len);
    resp->body_len = resp->body != NULL ? len : 0;
    if (resp->body == NULL)
        resp->status = 500;
}

/* New response document with "jsonrpc" and "id"; returns the root object. */
static yyjson_mut_val *rpc_begin(yyjson_mut_doc *doc, yyjson_val *id)
{
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    if (id != NULL)
        yyjson_mut_obj_add_val(doc, root, "id", yyjson_val_mut_copy(doc, id));
    else
        yyjson_mut_obj_add_null(doc, root, "id");
    return root;
}

static void rpc_error(av_http_resp *resp, yyjson_val *id, int code, const char *message)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = rpc_begin(doc, id);
    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_strcpy(doc, err, "message", message);
    yyjson_mut_obj_add_val(doc, root, "error", err);
    rpc_write(resp, doc);
    yyjson_mut_doc_free(doc);
}

/* Tool error result (isError: true) with a plain-text message. */
static void tool_error_text(av_http_resp *resp, yyjson_val *id, const char *text)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = rpc_begin(doc, id);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, content);
    yyjson_mut_obj_add_str(doc, item, "type", "text");
    yyjson_mut_obj_add_strcpy(doc, item, "text", text);
    yyjson_mut_obj_add_val(doc, res, "content", content);
    yyjson_mut_obj_add_bool(doc, res, "isError", true);
    yyjson_mut_obj_add_val(doc, root, "result", res);
    rpc_write(resp, doc);
    yyjson_mut_doc_free(doc);
}

/* ------------------------------------------------------------------ inner /v1 calls */

typedef struct {
    int status;
    char *body; /* malloc'd JSON (response or problem), may be NULL */
    size_t len;
} inner_result;

/*
 * Run one /v1 request through the router with the caller's credentials. path must be
 * percent-encoded. The result body is a JSON document (success) or a problem object (error).
 */
static void inner_call(const av_http_req *outer, const char *method, const char *path, const char *query,
                       const char *body, inner_result *out)
{
    memset(out, 0, sizeof *out);
    av_http_resp r;
    memset(&r, 0, sizeof r);
    r.request_id = outer->request_id;
    av_err e;
    av_err_clear(&e);

    const struct av_auth *auth = av_rest_get_auth();
    if (auth != NULL &&
        av_auth_check(auth, outer->authorization, av_auth_required_scope(method, path), NULL, &e) != AV_OK) {
        av_problem_from_err(&e, &r, path);
    } else if (g_router == NULL) {
        av_err_set(&e, "internal", "mcp router not initialised");
        av_problem_from_err(&e, &r, path);
    } else {
        av_route_match m;
        av_router_match(g_router, method, path, outer->arena, &m);
        if (m.status != 0 || m.bad_encoding) {
            av_err_set(&e, m.bad_encoding ? "bad_request" : "route_not_found", "no route for %s %s", method,
                       path);
            av_problem_from_err(&e, &r, path);
        } else {
            av_http_req in = *outer;
            in.method = method;
            in.path = path;
            in.query = query != NULL ? query : "";
            in.body = body != NULL ? body : "";
            in.body_len = strlen(in.body);
            in.content_type = "application/json";
            in.idempotency_key = NULL;
            m.handler(&in, m.params, m.n_params, &r, m.ctx);
        }
    }
    out->status = r.status != 0 ? r.status : 500;
    out->body = r.body;
    out->len = r.body_len;
}

/*
 * Tool result from an inner call: 2xx → structuredContent = the /v1 response object and a text
 * block (summary when given, else the JSON); otherwise isError with the problem object as text.
 */
static void tool_result_from(av_http_resp *resp, yyjson_val *id, const inner_result *ir, const char *summary)
{
    bool ok = ir->status >= 200 && ir->status < 300;
    yyjson_doc *parsed = ir->body != NULL ? yyjson_read(ir->body, ir->len, 0) : NULL;
    yyjson_val *obj = parsed != NULL ? yyjson_doc_get_root(parsed) : NULL;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = rpc_begin(doc, id);
    yyjson_mut_val *res = yyjson_mut_obj(doc);
    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, content);
    yyjson_mut_obj_add_str(doc, item, "type", "text");
    if (ok && summary != NULL) {
        yyjson_mut_obj_add_strcpy(doc, item, "text", summary);
    } else if (ir->body != NULL) {
        yyjson_mut_obj_add_strncpy(doc, item, "text", ir->body, ir->len);
    } else {
        char t[64];
        (void) snprintf(t, sizeof t, "HTTP %d", ir->status);
        yyjson_mut_obj_add_strcpy(doc, item, "text", t);
    }
    yyjson_mut_obj_add_val(doc, res, "content", content);
    if (ok && obj != NULL && yyjson_is_obj(obj))
        yyjson_mut_obj_add_val(doc, res, "structuredContent", yyjson_val_mut_copy(doc, obj));
    yyjson_mut_obj_add_bool(doc, res, "isError", !ok);
    yyjson_mut_obj_add_val(doc, root, "result", res);
    rpc_write(resp, doc);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(parsed);
}

/* Percent-encode one path segment (RFC 3986 unreserved characters pass through). */
static char *encode_segment(av_arena *a, const char *s)
{
    size_t n = strlen(s);
    char *out = av_arena_alloc(a, 3 * n + 1, 1);
    if (out == NULL)
        return NULL;
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char) s[i];
        if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            out[j++] = (char) ch;
        } else {
            out[j++] = '%';
            out[j++] = hex[ch >> 4];
            out[j++] = hex[ch & 15];
        }
    }
    out[j] = '\0';
    return out;
}

static const char *str_or(yyjson_val *v, const char *dflt)
{
    const char *s = yyjson_get_str(v);
    return s != NULL ? s : dflt;
}

static const char *arg_str(yyjson_val *args, const char *key)
{
    return yyjson_get_str(yyjson_obj_get(args, key));
}

/* Copy filter / where from the tool arguments into a /v1 request body. Error text or NULL. */
static const char *copy_filter(yyjson_mut_doc *doc, yyjson_mut_val *body, yyjson_val *args)
{
    yyjson_val *f = yyjson_obj_get(args, "filter");
    yyjson_val *w = yyjson_obj_get(args, "where");
    if (f != NULL && !yyjson_is_null(f) && w != NULL && !yyjson_is_null(w))
        return "pass at most one of 'filter' and 'where'";
    if (f != NULL && !yyjson_is_null(f)) {
        if (!yyjson_is_obj(f))
            return "'filter' must be a JSON filter object, e.g. {\"eq\": {\"topic\": \"pets\"}}";
        yyjson_mut_obj_add_val(doc, body, "filter", yyjson_val_mut_copy(doc, f));
    }
    if (w != NULL && !yyjson_is_null(w)) {
        if (!yyjson_is_str(w))
            return "'where' must be a string, e.g. \"stars >= 4 AND topic = str('pets')\"";
        if (yyjson_get_len(w) > 0)
            yyjson_mut_obj_add_strcpy(doc, body, "where", yyjson_get_str(w));
    }
    return NULL;
}

static char *write_body(yyjson_mut_doc *doc, av_arena *a)
{
    size_t len = 0;
    char *tmp = yyjson_mut_write(doc, 0, &len);
    if (tmp == NULL)
        return NULL;
    char *out = av_arena_alloc(a, len + 1, 1);
    if (out != NULL) {
        memcpy(out, tmp, len);
        out[len] = '\0';
    }
    free(tmp);
    return out;
}

/* ------------------------------------------------------------------ tool definitions */

typedef struct {
    const char *name, *type, *description;
} prop_def;

static void add_tool(yyjson_mut_doc *doc, yyjson_mut_val *tools, const char *name, const char *title,
                     const char *description, const prop_def *props, size_t n_props,
                     const char *const *required, size_t n_req, bool read_only)
{
    yyjson_mut_val *t = yyjson_mut_arr_add_obj(doc, tools);
    yyjson_mut_obj_add_str(doc, t, "name", name);
    yyjson_mut_obj_add_str(doc, t, "title", title);
    yyjson_mut_obj_add_str(doc, t, "description", description);
    yyjson_mut_val *schema = yyjson_mut_obj_add_obj(doc, t, "inputSchema");
    yyjson_mut_obj_add_str(doc, schema, "type", "object");
    yyjson_mut_val *p = yyjson_mut_obj_add_obj(doc, schema, "properties");
    for (size_t i = 0; i < n_props; i++) {
        yyjson_mut_val *o = yyjson_mut_obj_add_obj(doc, p, props[i].name);
        if (strcmp(props[i].type, "number[]") == 0) {
            yyjson_mut_obj_add_str(doc, o, "type", "array");
            yyjson_mut_val *items = yyjson_mut_obj_add_obj(doc, o, "items");
            yyjson_mut_obj_add_str(doc, items, "type", "number");
        } else if (strcmp(props[i].type, "entity[]") == 0) {
            yyjson_mut_obj_add_str(doc, o, "type", "array");
            yyjson_mut_obj_add_uint(doc, o, "maxItems", MCP_MAX_UPSERT);
            yyjson_mut_val *items = yyjson_mut_obj_add_obj(doc, o, "items");
            yyjson_mut_obj_add_str(doc, items, "type", "object");
        } else {
            yyjson_mut_obj_add_str(doc, o, "type", props[i].type);
        }
        if (strcmp(props[i].name, "k") == 0) {
            yyjson_mut_obj_add_int(doc, o, "minimum", 1);
            yyjson_mut_obj_add_int(doc, o, "maximum", MCP_MAX_SEARCH_K);
        }
        yyjson_mut_obj_add_str(doc, o, "description", props[i].description);
    }
    yyjson_mut_val *req = yyjson_mut_obj_add_arr(doc, schema, "required");
    for (size_t i = 0; i < n_req; i++)
        yyjson_mut_arr_add_str(doc, req, required[i]);
    yyjson_mut_val *ann = yyjson_mut_obj_add_obj(doc, t, "annotations");
    yyjson_mut_obj_add_bool(doc, ann, "readOnlyHint", read_only);
    if (!read_only) {
        yyjson_mut_obj_add_bool(doc, ann, "destructiveHint", false);
        yyjson_mut_obj_add_bool(doc, ann, "idempotentHint", true);
    }
    yyjson_mut_obj_add_bool(doc, ann, "openWorldHint", false);
}

static const prop_def k_coll = {"collection", "string", "Collection name (see arkivector_list_collections)."};
static const prop_def k_where = {"where", "string",
                                 "Arkiv query-language filter, e.g. \"stars >= 4 AND topic = str('pets')\". "
                                 "Strings need str('...'). Use at most one of where/filter."};
static const prop_def k_filter = {"filter", "object", "JSON filter, e.g. {\"eq\": {\"topic\": \"pets\"}}."};

static void handle_tools_list(av_http_resp *resp, yyjson_val *id)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = rpc_begin(doc, id);
    yyjson_mut_val *res = yyjson_mut_obj_add_obj(doc, root, "result");
    yyjson_mut_val *tools = yyjson_mut_obj_add_arr(doc, res, "tools");

    add_tool(doc, tools, "arkivector_list_collections", "List collections",
             "List the vector collections this daemon indexes: column type and dimension, index, declared "
             "attributes, live entity count and sync state. Call this first to learn collection names, the "
             "vector dimension a search needs, and which attributes can be filtered.",
             NULL, 0, NULL, 0, true);

    static const prop_def search_props[] = {
        {"collection", "string", "Collection name."},
        {"vector", "number[]", "Query embedding with exactly the collection's dimension."},
        {"text", "string",
         "Natural-language query. Needs an embedder configured on the daemon; without one, embed the "
         "text yourself and pass 'vector'."},
        {"k", "integer", "Number of results (1-50, default 10)."},
        {"where", "string", "Arkiv query-language filter, e.g. \"topic = str('pets')\"."},
        {"filter", "object", "JSON filter, e.g. {\"gte\": {\"stars\": 4}}."},
        {"include_document", "boolean", "Return each hit's stored document text (default false)."},
    };
    static const char *const search_req[] = {"collection"};
    add_tool(
        doc, tools, "arkivector_search", "Vector search",
        "Nearest-neighbour search in a collection with pgvector's distance operators, optionally filtered "
        "by attributes. Returns hits sorted by distance (smaller is closer) with score, attributes and, on "
        "request, the document.",
        search_props, sizeof search_props / sizeof search_props[0], search_req, 1, true);

    static const prop_def get_props[] = {
        {"collection", "string", "Collection name."},
        {"ref", "string", "Entity id (the av.id you stored) or Arkiv key (0x + 64 hex)."},
        {"include_document", "boolean", "Return the stored document text (default false)."},
    };
    static const char *const get_req[] = {"collection", "ref"};
    add_tool(
        doc, tools, "arkivector_get", "Get entity",
        "Fetch one live entity by id or key: attributes, owner, expiry block and optionally its document.",
        get_props, sizeof get_props / sizeof get_props[0], get_req, 2, true);

    prop_def count_props[] = {k_coll, k_where, k_filter};
    static const char *const count_req[] = {"collection"};
    add_tool(doc, tools, "arkivector_count", "Count entities",
             "Count the live entities in a collection, optionally only those matching a filter.", count_props,
             3, count_req, 1, true);

    if (av_rest_writes_enabled()) {
        static const prop_def up_props[] = {
            {"collection", "string", "Collection name."},
            {"entities", "entity[]",
             "Up to 100 entities: {\"id\": \"note-1\", \"vector\": [...], \"document\": \"text\", "
             "\"attributes\": {\"topic\": \"pets\"}, \"ttl\": {\"days\": 30}}. An existing id is updated."},
            {"wait", "string",
             "\"indexed\" (default: return once searchable), \"receipt\" (once mined) or \"none\"."},
        };
        static const char *const up_req[] = {"collection", "entities"};
        add_tool(
            doc, tools, "arkivector_upsert", "Upsert entities",
            "Write entities (vector, document, attributes) to the Arkiv chain through the daemon's wallet, "
            "upserting by id. Each call is an on-chain transaction that costs gas.",
            up_props, sizeof up_props / sizeof up_props[0], up_req, 2, false);
    }

    rpc_write(resp, doc);
    yyjson_mut_doc_free(doc);
}

/* ------------------------------------------------------------------ tool handlers */

static void tool_list_collections(const av_http_req *req, av_http_resp *resp, yyjson_val *id)
{
    inner_result ir;
    inner_call(req, "GET", "/v1/collections", "", NULL, &ir);
    char *summary = NULL;
    yyjson_doc *d = ir.body != NULL && ir.status == 200 ? yyjson_read(ir.body, ir.len, 0) : NULL;
    yyjson_val *arr = d != NULL ? yyjson_obj_get(yyjson_doc_get_root(d), "collections") : NULL;
    if (arr != NULL && yyjson_is_arr(arr)) {
        size_t cap = 256 + yyjson_arr_size(arr) * 320;
        summary = malloc(cap);
        if (summary != NULL) {
            size_t pos = (size_t) snprintf(summary, cap, "%zu collection(s)\n", yyjson_arr_size(arr));
            size_t i, max;
            yyjson_val *c;
            yyjson_arr_foreach(arr, i, max, c)
            {
                yyjson_val *col = yyjson_obj_get(c, "column");
                yyjson_val *ix = yyjson_obj_get(c, "index");
                yyjson_val *st = yyjson_obj_get(c, "stats");
                yyjson_val *sy = yyjson_obj_get(c, "sync");
                int n = snprintf(
                    summary + pos, cap - pos, "- %s: %s(%lld), index %s %s, %llu live entities, sync %s\n",
                    str_or(yyjson_obj_get(c, "name"), "?"), str_or(yyjson_obj_get(col, "type"), "?"),
                    (long long) yyjson_get_sint(yyjson_obj_get(col, "dimensions")),
                    str_or(yyjson_obj_get(ix, "method"), "?"), str_or(yyjson_obj_get(ix, "opclass"), ""),
                    (unsigned long long) yyjson_get_uint(yyjson_obj_get(st, "live_entities")),
                    str_or(yyjson_obj_get(sy, "state"), "?"));
                if (n < 0 || (size_t) n >= cap - pos)
                    break;
                pos += (size_t) n;
            }
        }
    }
    tool_result_from(resp, id, &ir, summary);
    free(summary);
    yyjson_doc_free(d);
    free(ir.body);
}

static void tool_search(const av_http_req *req, av_http_resp *resp, yyjson_val *id, yyjson_val *args)
{
    const char *coll = arg_str(args, "collection");
    if (coll == NULL || coll[0] == '\0') {
        tool_error_text(resp, id, "argument 'collection' (string) is required");
        return;
    }
    yyjson_val *vec = yyjson_obj_get(args, "vector");
    const char *text = arg_str(args, "text");
    if ((vec == NULL || yyjson_is_null(vec)) && text != NULL) {
        tool_error_text(
            resp, id,
            "text queries need an embedder, and none is configured on this daemon: embed the text "
            "with the collection's embedding model and pass 'vector'");
        return;
    }
    if (vec == NULL || !yyjson_is_arr(vec)) {
        tool_error_text(resp, id, "argument 'vector' (array of numbers) is required");
        return;
    }
    int64_t k = 10;
    yyjson_val *kv = yyjson_obj_get(args, "k");
    if (kv != NULL && !yyjson_is_null(kv)) {
        if (!yyjson_is_int(kv) || yyjson_get_sint(kv) < 1 || yyjson_get_sint(kv) > MCP_MAX_SEARCH_K) {
            tool_error_text(resp, id, "'k' must be an integer from 1 to 50");
            return;
        }
        k = yyjson_get_sint(kv);
    }
    yyjson_mut_doc *bd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = yyjson_mut_obj(bd);
    yyjson_mut_doc_set_root(bd, body);
    yyjson_mut_obj_add_val(bd, body, "vector", yyjson_val_mut_copy(bd, vec));
    yyjson_mut_obj_add_int(bd, body, "k", k);
    const char *ferr = copy_filter(bd, body, args);
    yyjson_mut_val *inc = yyjson_mut_obj_add_arr(bd, body, "include");
    yyjson_mut_arr_add_str(bd, inc, "attributes");
    if (yyjson_get_bool(yyjson_obj_get(args, "include_document")))
        yyjson_mut_arr_add_str(bd, inc, "document");
    char *json = ferr == NULL ? write_body(bd, req->arena) : NULL;
    yyjson_mut_doc_free(bd);
    if (ferr != NULL) {
        tool_error_text(resp, id, ferr);
        return;
    }
    const char *seg = encode_segment(req->arena, coll);
    char path[512];
    if (json == NULL || seg == NULL ||
        snprintf(path, sizeof path, "/v1/collections/%s/search", seg) >= (int) sizeof path) {
        tool_error_text(resp, id, "request too large");
        return;
    }
    inner_result ir;
    inner_call(req, "POST", path, "", json, &ir);
    tool_result_from(resp, id, &ir, NULL);
    free(ir.body);
}

static void tool_get(const av_http_req *req, av_http_resp *resp, yyjson_val *id, yyjson_val *args)
{
    const char *coll = arg_str(args, "collection");
    const char *ref = arg_str(args, "ref");
    if (coll == NULL || ref == NULL || coll[0] == '\0' || ref[0] == '\0') {
        tool_error_text(resp, id, "arguments 'collection' and 'ref' (strings) are required");
        return;
    }
    const char *cseg = encode_segment(req->arena, coll);
    const char *rseg = encode_segment(req->arena, ref);
    char path[1024];
    if (cseg == NULL || rseg == NULL ||
        snprintf(path, sizeof path, "/v1/collections/%s/entities/%s", cseg, rseg) >= (int) sizeof path) {
        tool_error_text(resp, id, "'ref' is too long");
        return;
    }
    const char *query =
        yyjson_get_bool(yyjson_obj_get(args, "include_document")) ? "include=attributes,document" : "";
    inner_result ir;
    inner_call(req, "GET", path, query, NULL, &ir);
    tool_result_from(resp, id, &ir, NULL);
    free(ir.body);
}

static void tool_count(const av_http_req *req, av_http_resp *resp, yyjson_val *id, yyjson_val *args)
{
    const char *coll = arg_str(args, "collection");
    if (coll == NULL || coll[0] == '\0') {
        tool_error_text(resp, id, "argument 'collection' (string) is required");
        return;
    }
    yyjson_mut_doc *bd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = yyjson_mut_obj(bd);
    yyjson_mut_doc_set_root(bd, body);
    const char *ferr = copy_filter(bd, body, args);
    char *json = ferr == NULL ? write_body(bd, req->arena) : NULL;
    yyjson_mut_doc_free(bd);
    if (ferr != NULL) {
        tool_error_text(resp, id, ferr);
        return;
    }
    const char *seg = encode_segment(req->arena, coll);
    char path[512];
    if (json == NULL || seg == NULL ||
        snprintf(path, sizeof path, "/v1/collections/%s/count", seg) >= (int) sizeof path) {
        tool_error_text(resp, id, "request too large");
        return;
    }
    inner_result ir;
    inner_call(req, "POST", path, "", json, &ir);
    tool_result_from(resp, id, &ir, NULL);
    free(ir.body);
}

static void tool_upsert(const av_http_req *req, av_http_resp *resp, yyjson_val *id, yyjson_val *args)
{
    if (!av_rest_writes_enabled()) {
        tool_error_text(resp, id, "writes are disabled: the daemon has no signing key (ARKIV_PRIVATE_KEY)");
        return;
    }
    const char *coll = arg_str(args, "collection");
    yyjson_val *ents = yyjson_obj_get(args, "entities");
    if (coll == NULL || coll[0] == '\0' || ents == NULL || !yyjson_is_arr(ents)) {
        tool_error_text(resp, id, "arguments 'collection' (string) and 'entities' (array) are required");
        return;
    }
    if (yyjson_arr_size(ents) == 0 || yyjson_arr_size(ents) > MCP_MAX_UPSERT) {
        tool_error_text(resp, id, "'entities' must hold 1 to 100 entities");
        return;
    }
    const char *wait = arg_str(args, "wait");
    if (wait == NULL)
        wait = "indexed";
    if (strcmp(wait, "indexed") != 0 && strcmp(wait, "receipt") != 0 && strcmp(wait, "none") != 0) {
        tool_error_text(resp, id, "'wait' must be \"indexed\", \"receipt\" or \"none\"");
        return;
    }
    yyjson_mut_doc *bd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = yyjson_mut_obj(bd);
    yyjson_mut_doc_set_root(bd, body);
    yyjson_mut_obj_add_val(bd, body, "entities", yyjson_val_mut_copy(bd, ents));
    yyjson_mut_obj_add_strcpy(bd, body, "wait", wait);
    char *json = write_body(bd, req->arena);
    yyjson_mut_doc_free(bd);
    const char *seg = encode_segment(req->arena, coll);
    char path[512];
    if (json == NULL || seg == NULL ||
        snprintf(path, sizeof path, "/v1/collections/%s/entities", seg) >= (int) sizeof path) {
        tool_error_text(resp, id, "request too large");
        return;
    }
    inner_result ir;
    inner_call(req, "POST", path, "", json, &ir);
    tool_result_from(resp, id, &ir, NULL);
    free(ir.body);
}

static void handle_tools_call(const av_http_req *req, av_http_resp *resp, yyjson_val *id, yyjson_val *params)
{
    const char *name = params != NULL ? yyjson_get_str(yyjson_obj_get(params, "name")) : NULL;
    if (name == NULL) {
        rpc_error(resp, id, -32602, "Invalid params: 'name' is required");
        return;
    }
    yyjson_val *args = yyjson_obj_get(params, "arguments");
    if (args != NULL && !yyjson_is_obj(args) && !yyjson_is_null(args)) {
        rpc_error(resp, id, -32602, "Invalid params: 'arguments' must be an object");
        return;
    }
    if (strcmp(name, "arkivector_list_collections") == 0)
        tool_list_collections(req, resp, id);
    else if (strcmp(name, "arkivector_search") == 0)
        tool_search(req, resp, id, args);
    else if (strcmp(name, "arkivector_get") == 0)
        tool_get(req, resp, id, args);
    else if (strcmp(name, "arkivector_count") == 0)
        tool_count(req, resp, id, args);
    else if (strcmp(name, "arkivector_upsert") == 0 && av_rest_writes_enabled())
        tool_upsert(req, resp, id, args);
    else
        rpc_error(resp, id, -32602, "Unknown tool");
}

static void handle_initialize(av_http_resp *resp, yyjson_val *id, yyjson_val *params)
{
    /* Version negotiation: answer the client's version if we support it, else our latest. */
    const char *want = params != NULL ? yyjson_get_str(yyjson_obj_get(params, "protocolVersion")) : NULL;
    const char *ver = MCP_LATEST_VERSION;
    for (size_t i = 0; want != NULL && i < sizeof k_versions / sizeof k_versions[0]; i++)
        if (strcmp(want, k_versions[i]) == 0)
            ver = k_versions[i];

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = rpc_begin(doc, id);
    yyjson_mut_val *res = yyjson_mut_obj_add_obj(doc, root, "result");
    yyjson_mut_obj_add_str(doc, res, "protocolVersion", ver);
    yyjson_mut_val *caps = yyjson_mut_obj_add_obj(doc, res, "capabilities");
    yyjson_mut_val *tools = yyjson_mut_obj_add_obj(doc, caps, "tools");
    yyjson_mut_obj_add_bool(doc, tools, "listChanged", false);
    yyjson_mut_val *srv = yyjson_mut_obj_add_obj(doc, res, "serverInfo");
    yyjson_mut_obj_add_str(doc, srv, "name", "arkivector");
    yyjson_mut_obj_add_str(doc, srv, "title", "Arkivector");
    yyjson_mut_obj_add_str(doc, srv, "version", AV_DAEMON_VERSION);
    yyjson_mut_obj_add_str(
        doc, res, "instructions",
        "Arkivector is vector search over data stored on the Arkiv chain. Call "
        "arkivector_list_collections first to learn collection names, vector dimensions and "
        "filterable attributes. arkivector_search needs a query vector of that dimension, "
        "produced by the collection's embedding model. Smaller distance means closer.");
    rpc_write(resp, doc);
    yyjson_mut_doc_free(doc);
}

/* ------------------------------------------------------------------ transport */

/* Loopback origins (any port) are always allowed; others only when API-key auth protects /mcp. */
static bool origin_allowed(const char *origin)
{
    if (origin == NULL || origin[0] == '\0' || strcmp(origin, "null") == 0)
        return origin == NULL || origin[0] == '\0' || av_rest_get_auth() != NULL;
    const char *p = strstr(origin, "://");
    if (p != NULL) {
        p += 3;
        static const char *const hosts[] = {"localhost", "127.0.0.1", "[::1]"};
        for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
            size_t n = strlen(hosts[i]);
            if (strncmp(p, hosts[i], n) == 0 && (p[n] == '\0' || p[n] == ':' || p[n] == '/'))
                return true;
        }
    }
    return av_rest_get_auth() != NULL;
}

/* POST /mcp */
static void h_mcp_post(const av_http_req *req, const char *const *params, size_t n_params, av_http_resp *resp,
                       void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;

    if (!origin_allowed(req->origin)) {
        av_err e;
        av_err_clear(&e);
        av_err_set(&e, "forbidden", "Origin %s is not allowed for /mcp (DNS rebinding protection)",
                   req->origin);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    yyjson_val *root = doc != NULL ? yyjson_doc_get_root(doc) : NULL;
    if (root == NULL) {
        rpc_error(resp, NULL, -32700, "Parse error");
        resp->status = 400;
        yyjson_doc_free(doc);
        return;
    }
    if (!yyjson_is_obj(root)) {
        /* JSON-RPC batches were removed in MCP 2025-06-18. */
        rpc_error(resp, NULL, -32600, "Invalid Request: send one JSON-RPC message per POST");
        resp->status = 400;
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *id = yyjson_obj_get(root, "id");
    const char *method = yyjson_get_str(yyjson_obj_get(root, "method"));
    yyjson_val *rpc_params = yyjson_obj_get(root, "params");

    if (method == NULL || id == NULL) {
        /* Notifications (no id) and responses to server requests: accepted, no body. */
        if (method == NULL && id == NULL && yyjson_obj_get(root, "result") == NULL &&
            yyjson_obj_get(root, "error") == NULL) {
            rpc_error(resp, NULL, -32600, "Invalid Request");
            resp->status = 400;
        } else {
            resp->status = 202;
            resp->body = NULL;
            resp->body_len = 0;
        }
        yyjson_doc_free(doc);
        return;
    }

    if (strcmp(method, "initialize") == 0)
        handle_initialize(resp, id, rpc_params);
    else if (strcmp(method, "ping") == 0) {
        yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = rpc_begin(out, id);
        yyjson_mut_obj_add_obj(out, r, "result");
        rpc_write(resp, out);
        yyjson_mut_doc_free(out);
    } else if (strcmp(method, "tools/list") == 0)
        handle_tools_list(resp, id);
    else if (strcmp(method, "tools/call") == 0)
        handle_tools_call(req, resp, id, rpc_params);
    else
        rpc_error(resp, id, -32601, "Method not found");

    yyjson_doc_free(doc);
}

void av_rest_register_mcp(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;
    g_router = r;
    av_err e;
    av_err_clear(&e);
    (void) av_router_add(r, "POST", "/mcp", h_mcp_post, NULL, &e);
}
