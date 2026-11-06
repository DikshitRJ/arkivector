/*
 * server/rest_collections.c: REST collection endpoints (/v1/collections, /v1/collections/{name},
 * /v1/collections:follow).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "arkiv/rpc.h"
#include "codec/hex.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "filter/value.h"
#include "server/json_io.h"
#include "server/problem.h"
#include "server/rest.h"
#include "sync/syncer.h"
#include "yyjson.h"

static void fail_internal(av_http_resp *resp, const char *msg, const char *instance)
{
    av_err e;
    av_err_set(&e, "internal", "%s", msg);
    av_problem_from_err(&e, resp, instance);
}

static yyjson_mut_val *format_collection(yyjson_mut_doc *doc, av_collection *c)
{
    const av_coldef *d = av_collection_def(c);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_strcpy(doc, obj, "name", d->name);
    yyjson_mut_obj_add_strcpy(doc, obj, "schema_tag", d->schema_tag);

    /* column */
    yyjson_mut_val *col = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, col, "type", av_vtype_name(d->vtype));
    yyjson_mut_obj_add_sint(doc, col, "dimensions", d->dimensions);
    yyjson_mut_obj_add_val(doc, obj, "column", col);

    /* index */
    yyjson_mut_val *ix = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, ix, "method", d->method == AV_INDEX_HNSW ? "hnsw" : "none");
    yyjson_mut_obj_add_str(doc, ix, "opclass", d->opclass ? d->opclass->name : "");
    yyjson_mut_val *with = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, with, "m", d->m);
    yyjson_mut_obj_add_int(doc, with, "ef_construction", d->ef_construction);
    yyjson_mut_obj_add_val(doc, ix, "with", with);
    yyjson_mut_obj_add_val(doc, obj, "index", ix);

    /* writers */
    yyjson_mut_val *w = yyjson_mut_arr(doc);
    for (size_t i = 0; i < d->n_writers; i++) {
        char hex[43];
        av_addr_to_hex(d->writers[i], hex);
        yyjson_mut_arr_add_strcpy(doc, w, hex);
    }
    yyjson_mut_obj_add_val(doc, obj, "writers", w);

    /* membership */
    const char *mstr = (d->membership == AV_MEMBERSHIP_CREATOR) ? "creator"
                       : (d->membership == AV_MEMBERSHIP_OWNER)   ? "owner"
                                                                 : "open";
    yyjson_mut_obj_add_str(doc, obj, "membership", mstr);

    /* attributes */
    yyjson_mut_val *at = yyjson_mut_obj(doc);
    for (size_t i = 0; i < d->n_attrs; i++) {
        yyjson_mut_obj_add_strcpy(doc, at, d->attrs[i].name, av_type_name((av_attr_type) d->attrs[i].type));
    }
    yyjson_mut_obj_add_val(doc, obj, "attributes", at);

    /* default_ttl */
    if (d->default_ttl.kind == AV_TTL_NONE) {
        yyjson_mut_obj_add_null(doc, obj, "default_ttl");
    } else {
        yyjson_mut_val *t = yyjson_mut_obj(doc);
        switch (d->default_ttl.kind) {
        case AV_TTL_BLOCKS:
            yyjson_mut_obj_add_sint(doc, t, "blocks", (int64_t) d->default_ttl.value);
            break;
        case AV_TTL_SECONDS:
            yyjson_mut_obj_add_sint(doc, t, "seconds", (int64_t) d->default_ttl.value);
            break;
        case AV_TTL_DAYS:
            yyjson_mut_obj_add_real(doc, t, "days", d->default_ttl.value);
            break;
        case AV_TTL_EXPIRES_AT:
            yyjson_mut_obj_add_sint(doc, t, "expires_at_block", (int64_t) d->default_ttl.value);
            break;
        default:
            break;
        }
        yyjson_mut_obj_add_val(doc, obj, "default_ttl", t);
    }

    /* model */
    if (d->model[0] != '\0') {
        yyjson_mut_obj_add_strcpy(doc, obj, "model", d->model);
    } else {
        yyjson_mut_obj_add_null(doc, obj, "model");
    }

    /* manifest_key */
    if (d->has_manifest_key) {
        char mk_hex[67];
        av_key_to_hex(d->manifest_key, mk_hex);
        yyjson_mut_obj_add_strcpy(doc, obj, "manifest_key", mk_hex);
    } else {
        yyjson_mut_obj_add_null(doc, obj, "manifest_key");
    }

    /* open */
    yyjson_mut_obj_add_bool(doc, obj, "open", d->membership == AV_MEMBERSHIP_OPEN);

    /* mode */
    yyjson_mut_obj_add_str(doc, obj, "mode", d->mode == AV_MODE_LITE ? "lite" : "full");

    /* stats */
    av_collection_stats stats;
    av_collection_get_stats(c, &stats);
    yyjson_mut_val *st = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, st, "live_entities", stats.live);
    yyjson_mut_obj_add_uint(doc, st, "segments", stats.segments);
    yyjson_mut_obj_add_uint(doc, st, "quarantined", stats.quarantined);
    yyjson_mut_obj_add_uint(doc, st, "tombstones", stats.tombstones);
    yyjson_mut_obj_add_val(doc, obj, "stats", st);

    /* sync */
    av_sync *sync = av_rest_get_sync();
    av_sync_coll_status sarr[32];
    size_t n_sync = sync != NULL ? av_sync_status_list(sync, sarr, 32) : 0;
    const av_sync_coll_status *found_sync = NULL;
    for (size_t i = 0; i < n_sync; i++) {
        if (strcmp(sarr[i].collection, d->name) == 0) {
            found_sync = &sarr[i];
            break;
        }
    }

    yyjson_mut_val *sy = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, sy, "name", d->name);
    if (found_sync != NULL) {
        yyjson_mut_obj_add_strcpy(doc, sy, "state", av_sync_state_name(found_sync->state));
        yyjson_mut_obj_add_uint(doc, sy, "as_of_block", found_sync->as_of_block);
        yyjson_mut_obj_add_uint(doc, sy, "head_block", found_sync->head_block);
        yyjson_mut_obj_add_uint(doc, sy, "lag_blocks", found_sync->lag_blocks);
        if (found_sync->last_error[0] != '\0') {
            yyjson_mut_obj_add_strcpy(doc, sy, "last_error", found_sync->last_error);
        } else {
            yyjson_mut_obj_add_null(doc, sy, "last_error");
        }
    } else {
        uint64_t as_of = av_collection_as_of(c);
        yyjson_mut_obj_add_str(doc, sy, "state", as_of > 0 ? "live" : "init");
        yyjson_mut_obj_add_uint(doc, sy, "as_of_block", as_of);
        yyjson_mut_obj_add_uint(doc, sy, "head_block", as_of);
        yyjson_mut_obj_add_uint(doc, sy, "lag_blocks", 0);
        yyjson_mut_obj_add_null(doc, sy, "last_error");
    }
    yyjson_mut_obj_add_val(doc, obj, "sync", sy);

    return obj;
}

/* GET /v1/collections */
typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *arr;
    uint64_t max_as_of;
    bool oom;
} list_acc;

static bool list_visit(av_collection *c, void *ctx)
{
    list_acc *acc = (list_acc *) ctx;
    yyjson_mut_val *co = format_collection(acc->doc, c);
    if (co == NULL || !yyjson_mut_arr_append(acc->arr, co)) {
        acc->oom = true;
        return false;
    }
    uint64_t as_of = av_collection_as_of(c);
    if (as_of > acc->max_as_of) {
        acc->max_as_of = as_of;
    }
    return true;
}

static void h_list_collections(const av_http_req *req, const char *const *params, size_t n_params,
                               av_http_resp *resp, void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        fail_internal(resp, "out of memory", req->path);
        return;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "collections", arr);

    list_acc acc = {doc, arr, 0, false};
    av_registry_each(reg, list_visit, &acc);

    if (acc.oom) {
        yyjson_mut_doc_free(doc);
        fail_internal(resp, "out of memory formatting collections", req->path);
        return;
    }

    resp->has_as_of = true;
    resp->as_of_block = acc.max_as_of;
    (void) av_resp_json(resp, 200, doc);
}

/* POST /v1/collections */
static void h_create_collection(const av_http_req *req, const char *const *params, size_t n_params,
                                av_http_resp *resp, void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = av_json_root_object(req_doc, &e);
    if (root == NULL) {
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_coldef def;
    if (av_coldef_from_json(root, req->arena, &def, &e) != AV_OK) {
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }
    yyjson_doc_free(req_doc);

    if (def.publish_manifest) {
        av_err_set(&e, "not_implemented", "publish_manifest is supported in Phase D5");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_collection *c = NULL;
    if (av_registry_add(reg, &def, &c, &e) != AV_OK) {
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *resp_doc = yyjson_mut_doc_new(NULL);
    if (resp_doc == NULL) {
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }

    yyjson_mut_val *co = format_collection(resp_doc, c);
    yyjson_mut_doc_set_root(resp_doc, co);

    resp->has_as_of = true;
    resp->as_of_block = av_collection_as_of(c);
    (void) av_resp_json(resp, 201, resp_doc);
    av_collection_release(c);
}

/* GET /v1/collections/{name} */
static void h_get_collection(const av_http_req *req, const char *const *params, size_t n_params,
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

    yyjson_mut_val *co = format_collection(doc, c);
    yyjson_mut_doc_set_root(doc, co);

    resp->has_as_of = true;
    resp->as_of_block = av_collection_as_of(c);
    (void) av_resp_json(resp, 200, doc);
    av_collection_release(c);
}

/* DELETE /v1/collections/{name} */
static void h_delete_collection(const av_http_req *req, const char *const *params, size_t n_params,
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

    av_err e;
    av_err_clear(&e);
    if (av_registry_remove(reg, name, &e) != AV_OK) {
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    resp->status = 204;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

/* Helper to parse an on-chain manifest entity into av_coldef */
static av_status parse_manifest_entity(const av_entity *ent, const uint8_t mkey[32], const char *override_name,
                                       av_arena *a, av_coldef *out, av_err *e)
{
    const av_attr *schema_attr = av_entity_attr(ent, AV_ATTR_SCHEMA);
    if (schema_attr == NULL || schema_attr->type != AV_AT_STR ||
        schema_attr->len != strlen("arkivector.v1.manifest") ||
        memcmp(schema_attr->val, "arkivector.v1.manifest", schema_attr->len) != 0) {
        av_err_set(e, "invalid_argument", "entity is not a collection manifest (arkiv.schema mismatch)");
        return AV_ERR;
    }

    /* Check if av.doc contains the CollectionCreate JSON directly */
    const av_attr *doc_attr = av_entity_attr(ent, AV_ATTR_DOC);
    if (doc_attr != NULL && doc_attr->len > 0) {
        if (av_coldef_from_text((const char *) doc_attr->val, doc_attr->len, a, false, out, e) == AV_OK) {
            if (override_name != NULL && override_name[0] != '\0') {
                (void) snprintf(out->name, sizeof(out->name), "%s", override_name);
            }
            memcpy(out->manifest_key, mkey, 32);
            out->has_manifest_key = true;
            return AV_OK;
        }
        av_err_clear(e);
    }

    /* Otherwise, parse from manifest attributes */
    memset(out, 0, sizeof(*out));
    out->membership = AV_MEMBERSHIP_CREATOR;
    out->mode = AV_MODE_FULL;
    out->m = 16;
    out->ef_construction = 64;

    const av_attr *name_attr = av_entity_attr(ent, "av.name");
    if (override_name != NULL && override_name[0] != '\0') {
        (void) snprintf(out->name, sizeof(out->name), "%s", override_name);
    } else if (name_attr != NULL && name_attr->len > 0 && name_attr->len <= AV_COLL_NAME_MAX) {
        memcpy(out->name, name_attr->val, name_attr->len);
        out->name[name_attr->len] = '\0';
    } else {
        av_err_set(e, "invalid_argument", "manifest entity missing av.name and no name provided");
        return AV_ERR;
    }

    const av_attr *tag_attr = av_entity_attr(ent, "av.tag");
    if (tag_attr != NULL && tag_attr->len > 0 && tag_attr->len <= AV_SCHEMA_TAG_MAX) {
        memcpy(out->schema_tag, tag_attr->val, tag_attr->len);
        out->schema_tag[tag_attr->len] = '\0';
    } else {
        (void) snprintf(out->schema_tag, sizeof(out->schema_tag), "arkivector.v1.%s", out->name);
    }

    /* av.coltype e.g. "halfvec(1536)" */
    const av_attr *coltype_attr = av_entity_attr(ent, "av.coltype");
    if (coltype_attr != NULL && coltype_attr->len > 0) {
        char ct[128];
        size_t ctl = coltype_attr->len < sizeof(ct) - 1 ? coltype_attr->len : sizeof(ct) - 1;
        memcpy(ct, coltype_attr->val, ctl);
        ct[ctl] = '\0';

        char *open_p = strchr(ct, '(');
        char *close_p = strchr(ct, ')');
        if (open_p != NULL && close_p != NULL && close_p > open_p) {
            *open_p = '\0';
            *close_p = '\0';
            out->dimensions = (int64_t) strtoll(open_p + 1, NULL, 10);
            if (strcmp(ct, "vector") == 0) {
                out->vtype = AV_VT_VECTOR;
            } else if (strcmp(ct, "halfvec") == 0) {
                out->vtype = AV_VT_HALFVEC;
            } else if (strcmp(ct, "bit") == 0) {
                out->vtype = AV_VT_BIT;
            } else if (strcmp(ct, "sparsevec") == 0) {
                out->vtype = AV_VT_SPARSEVEC;
            } else {
                av_err_set(e, "invalid_argument", "unknown column type '%s' in manifest", ct);
                return AV_ERR;
            }
        } else {
            av_err_set(e, "invalid_argument", "malformed av.coltype '%s' in manifest", ct);
            return AV_ERR;
        }
    } else {
        av_err_set(e, "invalid_argument", "manifest entity missing av.coltype");
        return AV_ERR;
    }

    /* av.index e.g. "hnsw (halfvec_cosine_ops) with (m = 16, ef_construction = 64)" or "none" */
    const av_attr *idx_attr = av_entity_attr(ent, "av.index");
    if (idx_attr != NULL && idx_attr->len > 0) {
        char itext[256];
        size_t itl = idx_attr->len < sizeof(itext) - 1 ? idx_attr->len : sizeof(itext) - 1;
        memcpy(itext, idx_attr->val, itl);
        itext[itl] = '\0';

        if (strncmp(itext, "none", 4) == 0) {
            out->method = AV_INDEX_NONE;
            out->opclass = av_opclass_for_operator(out->vtype, "<->");
            if (out->opclass == NULL && out->vtype == AV_VT_BIT) {
                out->opclass = av_opclass_for_operator(out->vtype, "<~>");
            }
        } else {
            out->method = AV_INDEX_HNSW;
            char *op_open = strchr(itext, '(');
            char *op_close = strchr(itext, ')');
            if (op_open != NULL && op_close != NULL && op_close > op_open) {
                *op_close = '\0';
                const char *op_name = op_open + 1;
                while (*op_name == ' ') op_name++;
                out->opclass = av_opclass_find(op_name);
                if (out->opclass == NULL) {
                    av_err_set(e, "invalid_argument", "unknown opclass '%s' in manifest", op_name);
                    return AV_ERR;
                }
            } else {
                out->opclass = av_opclass_for_operator(out->vtype, "<->");
            }

            char *m_pos = strstr(itext, "m = ");
            if (m_pos != NULL) {
                out->m = atoi(m_pos + 4);
            }
            char *ef_pos = strstr(itext, "ef_construction = ");
            if (ef_pos != NULL) {
                out->ef_construction = atoi(ef_pos + 18);
            }
        }
    } else {
        out->method = AV_INDEX_HNSW;
        out->opclass = av_opclass_for_operator(out->vtype, "<=>");
        if (out->opclass == NULL) {
            out->opclass = av_opclass_for_operator(out->vtype, "<->");
        }
    }

    /* av.writers: packed 20-byte addresses */
    const av_attr *wr_attr = av_entity_attr(ent, "av.writers");
    if (wr_attr != NULL && wr_attr->len > 0) {
        if (wr_attr->len % 20 != 0) {
            av_err_set(e, "invalid_argument", "manifest av.writers length %zu is not a multiple of 20", wr_attr->len);
            return AV_ERR;
        }
        size_t nw = wr_attr->len / 20;
        out->writers = av_arena_alloc(a, nw * 20, 1);
        if (out->writers == NULL) {
            av_err_set(e, "internal", "out of memory for manifest writers");
            return AV_ERR;
        }
        memcpy(out->writers, wr_attr->val, wr_attr->len);
        out->n_writers = nw;
    }

    /* av.model */
    const av_attr *model_attr = av_entity_attr(ent, "av.model");
    if (model_attr != NULL && model_attr->len > 0 && model_attr->len <= 128) {
        memcpy(out->model, model_attr->val, model_attr->len);
        out->model[model_attr->len] = '\0';
    }

    memcpy(out->manifest_key, mkey, 32);
    out->has_manifest_key = true;
    return AV_OK;
}

/* POST /v1/collections:follow */
static void h_follow_collection(const av_http_req *req, const char *const *params, size_t n_params,
                                av_http_resp *resp, void *ctx)
{
    (void) params;
    (void) n_params;
    (void) ctx;

    av_registry *reg = av_rest_get_registry();
    if (reg == NULL) {
        fail_internal(resp, "registry service not available", req->path);
        return;
    }

    av_err e;
    av_err_clear(&e);

    yyjson_doc *req_doc = NULL;
    if (av_json_parse(req->body, req->body_len, &req_doc, &e) != AV_OK) {
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *root = av_json_root_object(req_doc, &e);
    if (root == NULL) {
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    static const char *const allowed_follow[] = {"definition", "manifest_key", "name"};
    if (av_json_check_fields(root, allowed_follow, 3, "", &e) != AV_OK) {
        yyjson_doc_free(req_doc);
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_val *def_val = yyjson_obj_get(root, "definition");
    yyjson_val *mk_val = yyjson_obj_get(root, "manifest_key");
    yyjson_val *name_val = yyjson_obj_get(root, "name");

    if (def_val != NULL && mk_val != NULL) {
        yyjson_doc_free(req_doc);
        av_err_set(&e, "bad_request", "cannot specify both 'definition' and 'manifest_key'");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    if (def_val == NULL && mk_val == NULL) {
        yyjson_doc_free(req_doc);
        av_err_set(&e, "bad_request", "must specify either 'definition' or 'manifest_key'");
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    av_coldef def;
    memset(&def, 0, sizeof(def));

    if (def_val != NULL) {
        if (av_coldef_from_json(def_val, req->arena, &def, &e) != AV_OK) {
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        if (def.publish_manifest) {
            yyjson_doc_free(req_doc);
            av_err_set(&e, "not_implemented", "publish_manifest is supported in Phase D5");
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    } else {
        /* Follow by manifest key */
        const char *mk_str = yyjson_get_str(mk_val);
        if (mk_str == NULL || strlen(mk_str) != 66 || strncmp(mk_str, "0x", 2) != 0) {
            yyjson_doc_free(req_doc);
            av_err_set(&e, "invalid_argument", "manifest_key must be 0x followed by 64 hex characters");
            av_err_field(&e, "/manifest_key");
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        uint8_t mkey[32];
        if (!av_key_from_hex(mk_str, mkey)) {
            yyjson_doc_free(req_doc);
            av_err_set(&e, "invalid_argument", "invalid hex in manifest_key");
            av_err_field(&e, "/manifest_key");
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        const char *opt_name = name_val ? yyjson_get_str(name_val) : NULL;

        av_rpc *rpc = av_rest_get_rpc();
        if (rpc == NULL) {
            yyjson_doc_free(req_doc);
            av_err_set(&e, "not_ready", "RPC service not available");
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        av_entity ent;
        bool found = false;
        if (av_rpc_get_entity(rpc, mkey, req->arena, &ent, &found, &e) != AV_OK) {
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
        if (!found) {
            yyjson_doc_free(req_doc);
            av_err_set(&e, "entity_not_found", "manifest entity '%s' not found or expired", mk_str);
            av_problem_from_err(&e, resp, req->path);
            return;
        }

        if (parse_manifest_entity(&ent, mkey, opt_name, req->arena, &def, &e) != AV_OK) {
            yyjson_doc_free(req_doc);
            av_problem_from_err(&e, resp, req->path);
            return;
        }
    }
    yyjson_doc_free(req_doc);

    av_collection *c = NULL;
    if (av_registry_add(reg, &def, &c, &e) != AV_OK) {
        av_problem_from_err(&e, resp, req->path);
        return;
    }

    yyjson_mut_doc *resp_doc = yyjson_mut_doc_new(NULL);
    if (resp_doc == NULL) {
        av_collection_release(c);
        fail_internal(resp, "out of memory", req->path);
        return;
    }

    yyjson_mut_val *co = format_collection(resp_doc, c);
    yyjson_mut_doc_set_root(resp_doc, co);

    resp->has_as_of = true;
    resp->as_of_block = av_collection_as_of(c);
    (void) av_resp_json(resp, 201, resp_doc);
    av_collection_release(c);
}

void av_rest_register_collections(av_router *r, bool debug_routes, void *ctx)
{
    (void) debug_routes;
    (void) ctx;

    av_err e;
    av_err_clear(&e);

    (void) av_router_add(r, "GET", "/v1/collections", h_list_collections, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections", h_create_collection, NULL, &e);
    (void) av_router_add(r, "POST", "/v1/collections:follow", h_follow_collection, NULL, &e);
    (void) av_router_add(r, "GET", "/v1/collections/{name}", h_get_collection, NULL, &e);
    (void) av_router_add(r, "DELETE", "/v1/collections/{name}", h_delete_collection, NULL, &e);
}
