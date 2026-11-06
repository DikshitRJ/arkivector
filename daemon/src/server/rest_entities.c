/*
 * server/rest_entities.c: REST entity endpoints (point lookup, write stubs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "codec/base64.h"
#include "codec/hex.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/value.h"
#include "query/executor.h"
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

static yyjson_mut_val *format_typed_val(yyjson_mut_doc *doc, const av_attr *at, av_arena *arena)
{
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    const char *tname = av_type_name((av_attr_type) at->type);
    yyjson_mut_obj_add_str(doc, obj, "type", tname);

    if (at->type == AV_AT_BOOL) {
        bool val = (at->len >= 32 && at->val[31] != 0);
        yyjson_mut_obj_add_bool(doc, obj, "value", val);
    } else if (at->type == AV_AT_I32) {
        av_value v;
        if (av_value_from_stored(AV_AT_I32, at->val, at->len, &v)) {
            char buf[64];
            av_value_format(&v, buf, sizeof(buf));
            yyjson_mut_obj_add_int(doc, obj, "value", atoi(buf));
        } else {
            yyjson_mut_obj_add_int(doc, obj, "value", 0);
        }
    } else if (at->type == AV_AT_STR) {
        yyjson_mut_obj_add_strncpy(doc, obj, "value", (const char *) at->val, at->len);
    } else if (at->type == AV_AT_BYTES) {
        char *b64 = NULL;
        size_t b64_len = 0;
        av_err err;
        av_err_clear(&err);
        if (av_base64_encode(at->val, at->len, arena, &b64, &b64_len, &err) == AV_OK) {
            yyjson_mut_obj_add_strncpy(doc, obj, "value", b64, b64_len);
        } else {
            yyjson_mut_obj_add_strcpy(doc, obj, "value", "");
        }
    } else {
        av_value v;
        if (av_value_from_stored((av_attr_type) at->type, at->val, at->len, &v)) {
            char buf[AV_VALUE_FORMAT_MAX];
            av_value_format(&v, buf, sizeof(buf));
            yyjson_mut_obj_add_strcpy(doc, obj, "value", buf);
        } else {
            yyjson_mut_obj_add_strcpy(doc, obj, "value", "");
        }
    }
    return obj;
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
        char *token = strtok(buf, ",");
        while (token != NULL) {
            if (strcmp(token, "attributes") == 0) {
                include_flags |= AV_INCLUDE_ATTRIBUTES;
            } else if (strcmp(token, "document") == 0) {
                include_flags |= AV_INCLUDE_DOCUMENT;
            } else if (strcmp(token, "vector") == 0) {
                include_flags |= AV_INCLUDE_VECTOR;
            }
            token = strtok(NULL, ",");
        }
    }

    if (get_query_param(req->query, "verify", buf, sizeof(buf))) {
        if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
            av_collection_release(c);
            av_err e;
            av_err_set(&e, "not_implemented", "entity verify cross-check arrives in Phase D6");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    }

    int timeout_ms = 5000;
    if (get_query_param(req->query, "timeout_ms", buf, sizeof(buf))) {
        timeout_ms = atoi(buf);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 60000) timeout_ms = 60000;
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
            if (av_store_get_id(store, coll_name, ref, strlen(ref), key, &found_id, &e) == AV_OK && found_id) {
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
    const char *doc_val = NULL;
    size_t doc_len = 0;
    const uint8_t *vec_val = NULL;
    size_t vec_len = 0;

    yyjson_mut_val *attrs_obj = yyjson_mut_obj(doc);

    for (size_t i = 0; i < n_attrs; i++) {
        const av_attr *at = &attrs[i];
        if (strcmp(at->name, AV_ATTR_ID) == 0) {
            id_val = (const char *) at->val;
        } else if (strcmp(at->name, AV_ATTR_DOC) == 0) {
            doc_val = (const char *) at->val;
            doc_len = at->len;
        } else if (strcmp(at->name, AV_ATTR_VEC) == 0) {
            vec_val = at->val;
            vec_len = at->len;
        } else if (strncmp(at->name, "av.", 3) != 0 && strcmp(at->name, AV_ATTR_SCHEMA) != 0) {
            if (include_flags & AV_INCLUDE_ATTRIBUTES) {
                yyjson_mut_val *tv = format_typed_val(doc, at, req->arena);
                yyjson_mut_obj_add_val(doc, attrs_obj, at->name, tv);
            }
        }
    }

    if (id_val != NULL) {
        yyjson_mut_obj_add_strcpy(doc, eo, "id", id_val);
    } else {
        yyjson_mut_obj_add_null(doc, eo, "id");
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

/* 501 stubs for write operations */
static void h_write_not_implemented(const av_http_req *req, const char *const *params, size_t n_params,
                                    av_http_resp *resp, void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;

    av_err e;
    av_err_set(&e, "not_implemented", "managed entity writes are supported in Phase D5");
    av_problem_from_err(&e, resp, req->path);
}

void av_rest_register_entities(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;

    av_err e;
    av_err_clear(&e);

    /* Read route */
    (void) av_router_add(r, "GET", "/v1/collections/{name}/entities/{ref}", h_get_entity, NULL, &e);

    /* 501 Write routes (Phase D5) */
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities", h_write_not_implemented, NULL, &e);
    (void) av_router_add(r, "DELETE", "/v1/collections/{name}/entities/{ref}", h_write_not_implemented, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities:delete", h_write_not_implemented, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities:extend", h_write_not_implemented, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections/{name}/entities:prepare", h_write_not_implemented, NULL, &e);
}
