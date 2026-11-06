/*
 * server/json_io.c: yyjson helpers. See json_io.h.
 */
#include "server/json_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

av_status av_json_parse(const char *body, size_t len, yyjson_doc **doc, av_err *e)
{
    *doc = NULL;
    if (body == NULL || len == 0) {
        av_err_set(e, "bad_request", "request body is empty, expected a JSON document");
        return AV_ERR;
    }
    yyjson_read_err err;
    /* Flags 0: strict JSON. Without YYJSON_READ_INSITU yyjson copies the input and does not write to it. */
    yyjson_doc *d = yyjson_read_opts((char *) (uintptr_t) body, len, 0, NULL, &err);
    if (d == NULL) {
        av_err_set(e, "bad_request", "malformed JSON at byte %zu: %s", err.pos, err.msg);
        return AV_ERR;
    }
    *doc = d;
    return AV_OK;
}

yyjson_val *av_json_root_object(yyjson_doc *doc, av_err *e)
{
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == NULL || !yyjson_is_obj(root)) {
        av_err_set(e, "bad_request", "request body must be a JSON object");
        return NULL;
    }
    return root;
}

void av_json_pointer(char *out, size_t cap, const char *prefix, const char *key)
{
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    if (prefix != NULL) {
        for (const char *p = prefix; *p != '\0' && o + 1 < cap; p++) {
            out[o++] = *p;
        }
    }
    if (o + 1 < cap) {
        out[o++] = '/';
    }
    for (const char *p = key; *p != '\0' && o + 2 < cap; p++) {
        if (*p == '~') {
            out[o++] = '~';
            out[o++] = '0';
        } else if (*p == '/') {
            out[o++] = '~';
            out[o++] = '1';
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

av_status av_json_check_fields(yyjson_val *obj, const char *const *allowed, size_t n, const char *ptr_prefix,
                               av_err *e)
{
    yyjson_obj_iter it;
    yyjson_obj_iter_init(obj, &it);
    yyjson_val *k;
    while ((k = yyjson_obj_iter_next(&it)) != NULL) {
        const char *name = yyjson_get_str(k);
        size_t nlen = yyjson_get_len(k);
        bool ok = false;
        for (size_t i = 0; i < n && !ok; i++) {
            ok = strlen(allowed[i]) == nlen && memcmp(allowed[i], name, nlen) == 0;
        }
        if (!ok) {
            char key[96];
            size_t c = nlen < sizeof(key) - 1 ? nlen : sizeof(key) - 1;
            memcpy(key, name, c);
            key[c] = '\0';
            av_err_set(e, "bad_request", "unknown field '%s'", key);
            char ptr[160];
            av_json_pointer(ptr, sizeof(ptr), ptr_prefix, key);
            av_err_field(e, ptr);
            return AV_ERR;
        }
    }
    return AV_OK;
}

static av_status member(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required,
                        yyjson_val **out, bool *present, av_err *e)
{
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (present != NULL) {
        *present = v != NULL;
    }
    *out = v;
    if (v == NULL && required) {
        av_err_set(e, "bad_request", "missing required field '%s'", key);
        char ptr[160];
        av_json_pointer(ptr, sizeof(ptr), ptr_prefix, key);
        av_err_field(e, ptr);
        return AV_ERR;
    }
    return AV_OK;
}

static av_status wrong_type(const char *key, const char *ptr_prefix, const char *want, av_err *e)
{
    av_err_set(e, "bad_request", "field '%s' must be %s", key, want);
    char ptr[160];
    av_json_pointer(ptr, sizeof(ptr), ptr_prefix, key);
    av_err_field(e, ptr);
    return AV_ERR;
}

av_status av_json_get_string(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required,
                             const char **out, bool *present, av_err *e)
{
    yyjson_val *v;
    if (member(obj, key, ptr_prefix, required, &v, present, e) != AV_OK) {
        return AV_ERR;
    }
    if (v == NULL) {
        return AV_OK;
    }
    if (!yyjson_is_str(v)) {
        return wrong_type(key, ptr_prefix, "a string", e);
    }
    *out = yyjson_get_str(v);
    return AV_OK;
}

av_status av_json_get_bool(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required, bool *out,
                           bool *present, av_err *e)
{
    yyjson_val *v;
    if (member(obj, key, ptr_prefix, required, &v, present, e) != AV_OK) {
        return AV_ERR;
    }
    if (v == NULL) {
        return AV_OK;
    }
    if (!yyjson_is_bool(v)) {
        return wrong_type(key, ptr_prefix, "a boolean", e);
    }
    *out = yyjson_get_bool(v);
    return AV_OK;
}

av_status av_json_get_int(yyjson_val *obj, const char *key, const char *ptr_prefix, bool required,
                          int64_t min, int64_t max, int64_t *out, bool *present, av_err *e)
{
    yyjson_val *v;
    if (member(obj, key, ptr_prefix, required, &v, present, e) != AV_OK) {
        return AV_ERR;
    }
    if (v == NULL) {
        return AV_OK;
    }
    if (!yyjson_is_int(v)) {
        return wrong_type(key, ptr_prefix, "an integer", e);
    }
    int64_t n;
    if (yyjson_is_uint(v) && yyjson_get_uint(v) > (uint64_t) INT64_MAX) {
        n = INT64_MAX;
    } else {
        n = yyjson_get_sint(v);
    }
    if (n < min || n > max) {
        av_err_set(e, "invalid_argument", "field '%s' must be between %lld and %lld", key, (long long) min,
                   (long long) max);
        char ptr[160];
        av_json_pointer(ptr, sizeof(ptr), ptr_prefix, key);
        av_err_field(e, ptr);
        return AV_ERR;
    }
    *out = n;
    return AV_OK;
}

av_status av_resp_json(av_http_resp *resp, int status, yyjson_mut_doc *doc)
{
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (json == NULL) {
        return AV_ERR;
    }
    free(resp->body);
    resp->body = json;
    resp->body_len = len;
    resp->status = status;
    resp->content_type = "application/json";
    return AV_OK;
}

av_status av_resp_json_str(av_http_resp *resp, int status, const char *json)
{
    size_t n = strlen(json);
    char *copy = malloc(n + 1);
    if (copy == NULL) {
        return AV_ERR;
    }
    memcpy(copy, json, n + 1);
    free(resp->body);
    resp->body = copy;
    resp->body_len = n;
    resp->status = status;
    resp->content_type = "application/json";
    return AV_OK;
}
