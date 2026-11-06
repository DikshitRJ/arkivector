/*
 * core/coldef.c: collection definition JSON parse / validate / serialize.
 * Contract: spec/openapi.yaml CollectionCreate, docs/api/collections.md.
 */
#include "core/coldef.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/vectorbytes.h"
#include "yyjson.h"

#define MAX_BLOCK_TTL   7884000
#define MAX_SECONDS_TTL 15768000
#define MAX_DAYS_TTL    182.5
#define MAX_JSON_INT    9007199254740991LL

static av_status fail(av_err *e, const char *code, const char *field, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static av_status fail(av_err *e, const char *code, const char *field, const char *fmt, ...)
{
    if (e != NULL) {
        va_list ap;
        va_start(ap, fmt);
        char msg[sizeof e->msg];
        (void) vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        av_err_set(e, code, "%s", msg);
        av_err_field(e, field);
    }
    return AV_ERR;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *s, size_t nbytes, uint8_t *out)
{
    if (s[0] != '0' || s[1] != 'x' || strlen(s) != 2 + 2 * nbytes)
        return false;
    for (size_t i = 0; i < nbytes; i++) {
        int h = hexval(s[2 + 2 * i]), l = hexval(s[3 + 2 * i]);
        if (h < 0 || l < 0)
            return false;
        out[i] = (uint8_t) (h * 16 + l);
    }
    return true;
}

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < n; i++) {
        out[2 + 2 * i] = digits[b[i] >> 4];
        out[3 + 2 * i] = digits[b[i] & 15];
    }
    out[2 + 2 * n] = '\0';
}

/* ----------------------------------------------------------- field helpers */

static bool key_is(const char *k, const char *name)
{
    return strcmp(k, name) == 0;
}

/* Integer in [lo, hi]; bad_request on a non-integer JSON value, invalid_argument when out of range. */
static av_status get_int(yyjson_val *v, const char *field, int64_t lo, int64_t hi, int64_t *out, av_err *e)
{
    if (!yyjson_is_int(v))
        return fail(e, "bad_request", field, "%s must be an integer", field);
    if (yyjson_is_uint(v) && yyjson_get_uint(v) > (uint64_t) INT64_MAX)
        return fail(e, "invalid_argument", field, "%s is out of range", field);
    int64_t x = yyjson_get_sint(v);
    if (x < lo || x > hi)
        return fail(e, "invalid_argument", field, "%s must be between %lld and %lld", field, (long long) lo,
                    (long long) hi);
    *out = x;
    return AV_OK;
}

static av_status get_str(yyjson_val *v, const char *field, size_t maxlen, const char **out, av_err *e)
{
    if (!yyjson_is_str(v))
        return fail(e, "bad_request", field, "%s must be a string", field);
    if (yyjson_get_len(v) > maxlen)
        return fail(e, "invalid_argument", field, "%s exceeds %zu bytes", field, maxlen);
    *out = yyjson_get_str(v);
    return AV_OK;
}

static av_status check_name(const char *s, size_t len, av_err *e)
{
    bool ok = len >= 1 && len <= AV_COLL_NAME_MAX && s[0] >= 'a' && s[0] <= 'z';
    for (size_t i = 1; ok && i < len; i++) {
        char c = s[i];
        ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    }
    if (!ok)
        return fail(e, "invalid_argument", "/name", "name must match ^[a-z][a-z0-9._-]{0,62}$");
    return AV_OK;
}

static bool parse_attr_type(const char *s, av_attr_type *t)
{
    static const struct {
        const char *n;
        av_attr_type t;
    } tab[] = {{"bool", AV_AT_BOOL}, {"i32", AV_AT_I32}, {"u64", AV_AT_U64},
               {"u256", AV_AT_U256}, {"dec", AV_AT_DEC}, {"str", AV_AT_STR},
               {"addr", AV_AT_ADDR}, {"key", AV_AT_KEY}, {"bytes", AV_AT_BYTES}};
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].n, s) == 0) {
            *t = tab[i].t;
            return true;
        }
    return false;
}

static const char *attr_type_name(av_attr_type t)
{
    switch (t) {
    case AV_AT_BOOL:
        return "bool";
    case AV_AT_I32:
        return "i32";
    case AV_AT_U64:
        return "u64";
    case AV_AT_U256:
        return "u256";
    case AV_AT_DEC:
        return "dec";
    case AV_AT_STR:
        return "str";
    case AV_AT_ADDR:
        return "addr";
    case AV_AT_KEY:
        return "key";
    case AV_AT_BYTES:
        return "bytes";
    case AV_AT_TOMBSTONE:
    default:
        return "?";
    }
}

static int addr_cmp(const void *a, const void *b)
{
    return memcmp(a, b, 20);
}

static int attr_cmp(const void *a, const void *b)
{
    return strcmp(((const av_schema_attr *) a)->name, ((const av_schema_attr *) b)->name);
}

/* ------------------------------------------------------------------- parse */

static av_status parse_column(yyjson_val *col, av_coldef *d, av_err *e)
{
    if (!yyjson_is_obj(col))
        return fail(e, "bad_request", "/column", "column must be an object");
    yyjson_val *type = NULL, *dims = NULL;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(col, idx, max, k, v)
    {
        const char *ks = yyjson_get_str(k);
        if (key_is(ks, "type"))
            type = v;
        else if (key_is(ks, "dimensions"))
            dims = v;
        else
            return fail(e, "bad_request", "/column", "unknown field '%s' in column", ks);
    }
    if (type == NULL || dims == NULL)
        return fail(e, "bad_request", "/column", "column needs type and dimensions");
    const char *ts;
    if (get_str(type, "/column/type", 16, &ts, e) != AV_OK)
        return AV_ERR;
    if (!av_vtype_parse(ts, &d->vtype))
        return fail(e, "invalid_argument", "/column/type", "unknown vector type '%s'", ts);
    if (get_int(dims, "/column/dimensions", 1, 1000000000, &d->dimensions, e) != AV_OK)
        return AV_ERR;
    size_t sz;
    if (!av_vb_encoded_size(d->vtype, d->dimensions, 0, &sz))
        return fail(e, "invalid_argument", "/column/dimensions", "dimensions %lld exceed the limit of %s",
                    (long long) d->dimensions, av_vtype_name(d->vtype));
    return AV_OK;
}

static av_status parse_index(yyjson_val *ix, av_coldef *d, av_err *e)
{
    if (!yyjson_is_obj(ix))
        return fail(e, "bad_request", "/index", "index must be an object");
    yyjson_val *method = NULL, *opclass = NULL, *with = NULL;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(ix, idx, max, k, v)
    {
        const char *ks = yyjson_get_str(k);
        if (key_is(ks, "method"))
            method = v;
        else if (key_is(ks, "opclass"))
            opclass = v;
        else if (key_is(ks, "with"))
            with = v;
        else
            return fail(e, "bad_request", "/index", "unknown field '%s' in index", ks);
    }
    if (method == NULL || opclass == NULL)
        return fail(e, "bad_request", "/index", "index needs method and opclass");
    const char *s;
    if (get_str(method, "/index/method", 8, &s, e) != AV_OK)
        return AV_ERR;
    if (strcmp(s, "hnsw") == 0)
        d->method = AV_INDEX_HNSW;
    else if (strcmp(s, "none") == 0)
        d->method = AV_INDEX_NONE;
    else
        return fail(e, "invalid_argument", "/index/method", "index method must be hnsw or none");
    if (get_str(opclass, "/index/opclass", 64, &s, e) != AV_OK)
        return AV_ERR;
    d->opclass = av_opclass_find(s);
    if (d->opclass == NULL)
        return fail(e, "invalid_argument", "/index/opclass", "unknown opclass '%s'", s);
    if (d->opclass->type != d->vtype)
        return fail(e, "invalid_argument", "/index/opclass", "opclass %s does not match column type %s", s,
                    av_vtype_name(d->vtype));
    d->m = 16;
    d->ef_construction = 64;
    if (with != NULL) {
        if (!yyjson_is_obj(with))
            return fail(e, "bad_request", "/index/with", "with must be an object");
        yyjson_obj_foreach(with, idx, max, k, v)
        {
            const char *ks = yyjson_get_str(k);
            int64_t x;
            if (key_is(ks, "m")) {
                if (get_int(v, "/index/with/m", 2, 100, &x, e) != AV_OK)
                    return AV_ERR;
                d->m = (int) x;
            } else if (key_is(ks, "ef_construction")) {
                if (get_int(v, "/index/with/ef_construction", 4, 1000, &x, e) != AV_OK)
                    return AV_ERR;
                d->ef_construction = (int) x;
            } else {
                return fail(e, "bad_request", "/index/with", "unknown field '%s' in with", ks);
            }
        }
    }
    if (d->ef_construction < 2 * d->m)
        return fail(e, "invalid_argument", "/index/with/ef_construction",
                    "ef_construction must be at least 2*m (%d)", 2 * d->m);
    return AV_OK;
}

static av_status parse_ttl(yyjson_val *t, av_col_ttl *out, av_err *e)
{
    if (yyjson_is_null(t)) {
        out->kind = AV_TTL_NONE;
        out->value = 0;
        return AV_OK;
    }
    if (!yyjson_is_obj(t) || yyjson_obj_size(t) != 1)
        return fail(e, "bad_request", "/default_ttl", "ttl must be an object with exactly one key");
    yyjson_val *k = unsafe_yyjson_get_first(t);
    yyjson_val *v = k + 1;
    const char *ks = yyjson_get_str(k);
    int64_t x;
    if (key_is(ks, "blocks")) {
        if (get_int(v, "/default_ttl/blocks", 1, MAX_BLOCK_TTL, &x, e) != AV_OK)
            return AV_ERR;
        out->kind = AV_TTL_BLOCKS;
        out->value = (double) x;
    } else if (key_is(ks, "seconds")) {
        if (get_int(v, "/default_ttl/seconds", 2, MAX_SECONDS_TTL, &x, e) != AV_OK)
            return AV_ERR;
        if (x % 2 != 0)
            return fail(e, "invalid_argument", "/default_ttl/seconds", "seconds must be even (2 s blocks)");
        out->kind = AV_TTL_SECONDS;
        out->value = (double) x;
    } else if (key_is(ks, "days")) {
        if (!yyjson_is_num(v))
            return fail(e, "bad_request", "/default_ttl/days", "days must be a number");
        double dv = yyjson_get_num(v);
        if (!(dv > 0.0) || dv > MAX_DAYS_TTL || isnan(dv))
            return fail(e, "invalid_argument", "/default_ttl/days", "days must be in (0, 182.5]");
        out->kind = AV_TTL_DAYS;
        out->value = dv;
    } else if (key_is(ks, "expires_at_block")) {
        if (get_int(v, "/default_ttl/expires_at_block", 0, MAX_JSON_INT, &x, e) != AV_OK)
            return AV_ERR;
        out->kind = AV_TTL_EXPIRES_AT;
        out->value = (double) x;
    } else {
        return fail(e, "bad_request", "/default_ttl", "unknown ttl unit '%s'", ks);
    }
    return AV_OK;
}

static av_status parse_writers(yyjson_val *arr, av_arena *a, av_coldef *d, av_err *e)
{
    if (!yyjson_is_arr(arr))
        return fail(e, "bad_request", "/writers", "writers must be an array");
    size_t n = yyjson_arr_size(arr);
    if (n > AV_MAX_WRITERS)
        return fail(e, "invalid_argument", "/writers", "at most %d writers are allowed", AV_MAX_WRITERS);
    d->writers = av_arena_alloc(a, (n ? n : 1) * 20, 1);
    if (d->writers == NULL)
        return fail(e, "internal", "/writers", "arena exhausted");
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(arr, idx, max, v)
    {
        char field[40];
        (void) snprintf(field, sizeof field, "/writers/%zu", idx);
        if (!yyjson_is_str(v))
            return fail(e, "bad_request", field, "writer must be a string");
        if (!parse_hex(yyjson_get_str(v), 20, d->writers[idx]))
            return fail(e, "invalid_argument", field, "writer must be 0x followed by 40 hex digits");
    }
    qsort(d->writers, n, 20, addr_cmp);
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (w == 0 || memcmp(d->writers[w - 1], d->writers[i], 20) != 0)
            memcpy(d->writers[w++], d->writers[i], 20);
    d->n_writers = w;
    return AV_OK;
}

static av_status parse_attrs(yyjson_val *obj, av_arena *a, av_coldef *d, av_err *e)
{
    if (!yyjson_is_obj(obj))
        return fail(e, "bad_request", "/attributes", "attributes must be an object");
    size_t n = yyjson_obj_size(obj);
    if (n > AV_MAX_SCHEMA_ATTRS)
        return fail(e, "invalid_argument", "/attributes", "at most %d schema attributes are allowed",
                    AV_MAX_SCHEMA_ATTRS);
    d->attrs = av_arena_calloc(a, (n ? n : 1) * sizeof(av_schema_attr), 8);
    if (d->attrs == NULL)
        return fail(e, "internal", "/attributes", "arena exhausted");
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(obj, idx, max, k, v)
    {
        const char *name = yyjson_get_str(k);
        size_t nl = yyjson_get_len(k);
        if (nl == 0 || nl > AV_ATTR_NAME_MAX)
            return fail(e, "invalid_argument", "/attributes", "attribute names must be 1..%d bytes",
                        AV_ATTR_NAME_MAX);
        for (size_t i = 0; i < nl; i++)
            if (!isprint((unsigned char) name[i]) || name[i] == '/')
                return fail(e, "invalid_argument", "/attributes",
                            "attribute name '%s' has invalid characters", name);
        if (name[0] == '$')
            return fail(e, "invalid_argument", "/attributes", "attribute names must not start with '$'");
        const char *ts;
        if (!yyjson_is_str(v))
            return fail(e, "bad_request", "/attributes", "attribute type must be a string");
        ts = yyjson_get_str(v);
        if (!parse_attr_type(ts, &d->attrs[idx].type))
            return fail(e, "invalid_argument", "/attributes", "attribute '%s' has unknown type '%s'", name,
                        ts);
        memcpy(d->attrs[idx].name, name, nl);
    }
    qsort(d->attrs, n, sizeof(av_schema_attr), attr_cmp);
    for (size_t i = 1; i < n; i++)
        if (strcmp(d->attrs[i - 1].name, d->attrs[i].name) == 0)
            return fail(e, "bad_request", "/attributes", "duplicate attribute '%s'", d->attrs[i].name);
    d->n_attrs = n;
    return AV_OK;
}

av_status av_coldef_from_json_ex(struct yyjson_val *obj, av_arena *a, bool internal, av_coldef *out,
                                 av_err *e)
{
    av_err_clear(e);
    if (!yyjson_is_obj(obj))
        return fail(e, "bad_request", "", "collection definition must be a JSON object");
    memset(out, 0, sizeof *out);
    out->membership = AV_MEMBERSHIP_CREATOR;
    out->mode = AV_MODE_FULL;
    yyjson_val *name = NULL, *tag = NULL, *col = NULL, *ix = NULL, *writers = NULL, *memb = NULL,
               *attrs = NULL, *ttl = NULL, *pm = NULL, *model = NULL, *mode = NULL, *mk = NULL;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(obj, idx, max, k, v)
    {
        const char *ks = yyjson_get_str(k);
        if (key_is(ks, "name"))
            name = v;
        else if (key_is(ks, "schema_tag"))
            tag = v;
        else if (key_is(ks, "column"))
            col = v;
        else if (key_is(ks, "index"))
            ix = v;
        else if (key_is(ks, "writers"))
            writers = v;
        else if (key_is(ks, "membership"))
            memb = v;
        else if (key_is(ks, "attributes"))
            attrs = v;
        else if (key_is(ks, "default_ttl"))
            ttl = v;
        else if (key_is(ks, "publish_manifest"))
            pm = v;
        else if (key_is(ks, "model"))
            model = v;
        else if (key_is(ks, "mode"))
            mode = v;
        else if (internal && key_is(ks, "manifest_key"))
            mk = v;
        else
            return fail(e, "bad_request", "", "unknown field '%s'", ks);
    }
    if (name == NULL || col == NULL || ix == NULL)
        return fail(e, "bad_request", "", "name, column and index are required");
    const char *s;
    if (!yyjson_is_str(name))
        return fail(e, "bad_request", "/name", "name must be a string");
    if (check_name(yyjson_get_str(name), yyjson_get_len(name), e) != AV_OK)
        return AV_ERR;
    (void) snprintf(out->name, sizeof out->name, "%s", yyjson_get_str(name));
    if (tag != NULL) {
        if (get_str(tag, "/schema_tag", AV_SCHEMA_TAG_MAX, &s, e) != AV_OK)
            return AV_ERR;
        if (s[0] == '\0')
            return fail(e, "invalid_argument", "/schema_tag", "schema_tag must not be empty");
        (void) snprintf(out->schema_tag, sizeof out->schema_tag, "%s", s);
    } else {
        (void) snprintf(out->schema_tag, sizeof out->schema_tag, "arkivector.v1.%s", out->name);
    }
    if (parse_column(col, out, e) != AV_OK || parse_index(ix, out, e) != AV_OK)
        return AV_ERR;
    if (memb != NULL) {
        if (get_str(memb, "/membership", 16, &s, e) != AV_OK)
            return AV_ERR;
        if (strcmp(s, "creator") == 0)
            out->membership = AV_MEMBERSHIP_CREATOR;
        else if (strcmp(s, "owner") == 0)
            out->membership = AV_MEMBERSHIP_OWNER;
        else if (strcmp(s, "open") == 0)
            out->membership = AV_MEMBERSHIP_OPEN;
        else
            return fail(e, "invalid_argument", "/membership", "membership must be creator, owner or open");
    }
    if (writers != NULL && parse_writers(writers, a, out, e) != AV_OK)
        return AV_ERR;
    if (out->membership == AV_MEMBERSHIP_OPEN && out->n_writers > 0)
        return fail(e, "invalid_argument", "/writers", "membership 'open' requires an empty writers list");
    if (out->membership != AV_MEMBERSHIP_OPEN && out->n_writers == 0)
        return fail(e, "invalid_argument", "/writers", "writers are required unless membership is 'open'");
    if (attrs != NULL && parse_attrs(attrs, a, out, e) != AV_OK)
        return AV_ERR;
    if (ttl != NULL && parse_ttl(ttl, &out->default_ttl, e) != AV_OK)
        return AV_ERR;
    if (pm != NULL) {
        if (!yyjson_is_bool(pm))
            return fail(e, "bad_request", "/publish_manifest", "publish_manifest must be a boolean");
        out->publish_manifest = yyjson_get_bool(pm);
    }
    if (model != NULL && !yyjson_is_null(model)) {
        if (get_str(model, "/model", 128, &s, e) != AV_OK)
            return AV_ERR;
        (void) snprintf(out->model, sizeof out->model, "%s", s);
    }
    if (mode != NULL) {
        if (get_str(mode, "/mode", 8, &s, e) != AV_OK)
            return AV_ERR;
        if (strcmp(s, "full") == 0)
            out->mode = AV_MODE_FULL;
        else if (strcmp(s, "lite") == 0)
            out->mode = AV_MODE_LITE;
        else
            return fail(e, "invalid_argument", "/mode", "mode must be full or lite");
    }
    if (mk != NULL && !yyjson_is_null(mk)) {
        if (!yyjson_is_str(mk) || !parse_hex(yyjson_get_str(mk), 32, out->manifest_key))
            return fail(e, "invalid_argument", "/manifest_key",
                        "manifest_key must be 0x followed by 64 hex digits");
        out->has_manifest_key = true;
    }
    return AV_OK;
}

av_status av_coldef_from_json(struct yyjson_val *obj, av_arena *a, av_coldef *out, av_err *e)
{
    return av_coldef_from_json_ex(obj, a, false, out, e);
}

av_status av_coldef_from_text(const char *json, size_t len, av_arena *a, bool internal, av_coldef *out,
                              av_err *e)
{
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *) json, len, 0, NULL, &err);
    if (doc == NULL)
        return fail(e, "bad_request", "", "invalid JSON: %s at byte %zu", err.msg, (size_t) err.pos);
    av_status st = av_coldef_from_json_ex(yyjson_doc_get_root(doc), a, internal, out, e);
    yyjson_doc_free(doc);
    return st;
}

/* ------------------------------------------------------------------ clone */

av_status av_coldef_clone(const av_coldef *src, av_coldef *dst, av_err *e)
{
    *dst = *src;
    dst->writers = NULL;
    dst->attrs = NULL;
    if (src->n_writers > 0) {
        dst->writers = malloc(src->n_writers * 20);
        if (dst->writers == NULL)
            goto oom;
        memcpy(dst->writers, src->writers, src->n_writers * 20);
    }
    if (src->n_attrs > 0) {
        dst->attrs = malloc(src->n_attrs * sizeof(av_schema_attr));
        if (dst->attrs == NULL) {
            free(dst->writers);
            dst->writers = NULL;
            goto oom;
        }
        memcpy(dst->attrs, src->attrs, src->n_attrs * sizeof(av_schema_attr));
    }
    return AV_OK;
oom:
    av_err_set(e, "internal", "out of memory cloning collection definition");
    return AV_ERR;
}

void av_coldef_release(av_coldef *d)
{
    if (d == NULL)
        return;
    free(d->writers);
    free(d->attrs);
    d->writers = NULL;
    d->attrs = NULL;
    d->n_writers = d->n_attrs = 0;
}

/* -------------------------------------------------------------- serialize */

char *av_coldef_to_json(const av_coldef *d, size_t *len)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "name", d->name);
    yyjson_mut_obj_add_strcpy(doc, root, "schema_tag", d->schema_tag);
    yyjson_mut_val *col = yyjson_mut_obj_add_obj(doc, root, "column");
    yyjson_mut_obj_add_str(doc, col, "type", av_vtype_name(d->vtype));
    yyjson_mut_obj_add_sint(doc, col, "dimensions", d->dimensions);
    yyjson_mut_val *ix = yyjson_mut_obj_add_obj(doc, root, "index");
    yyjson_mut_obj_add_str(doc, ix, "method", d->method == AV_INDEX_HNSW ? "hnsw" : "none");
    yyjson_mut_obj_add_str(doc, ix, "opclass", d->opclass->name);
    yyjson_mut_val *with = yyjson_mut_obj_add_obj(doc, ix, "with");
    yyjson_mut_obj_add_sint(doc, with, "m", d->m);
    yyjson_mut_obj_add_sint(doc, with, "ef_construction", d->ef_construction);
    yyjson_mut_val *w = yyjson_mut_obj_add_arr(doc, root, "writers");
    for (size_t i = 0; i < d->n_writers; i++) {
        char hex[43];
        to_hex(d->writers[i], 20, hex);
        yyjson_mut_arr_add_strcpy(doc, w, hex);
    }
    yyjson_mut_obj_add_str(doc, root, "membership",
                           d->membership == AV_MEMBERSHIP_CREATOR ? "creator"
                           : d->membership == AV_MEMBERSHIP_OWNER ? "owner"
                                                                  : "open");
    yyjson_mut_val *at = yyjson_mut_obj_add_obj(doc, root, "attributes");
    for (size_t i = 0; i < d->n_attrs; i++)
        yyjson_mut_obj_add(at, yyjson_mut_strcpy(doc, d->attrs[i].name),
                           yyjson_mut_str(doc, attr_type_name(d->attrs[i].type)));
    if (d->default_ttl.kind != AV_TTL_NONE) {
        yyjson_mut_val *t = yyjson_mut_obj_add_obj(doc, root, "default_ttl");
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
        case AV_TTL_NONE:
        default:
            break;
        }
    }
    yyjson_mut_obj_add_bool(doc, root, "publish_manifest", d->publish_manifest);
    if (d->model[0] != '\0')
        yyjson_mut_obj_add_strcpy(doc, root, "model", d->model);
    yyjson_mut_obj_add_str(doc, root, "mode", d->mode == AV_MODE_LITE ? "lite" : "full");
    if (d->has_manifest_key) {
        char hex[67];
        to_hex(d->manifest_key, 32, hex);
        yyjson_mut_obj_add_strcpy(doc, root, "manifest_key", hex);
    }
    size_t n = 0;
    char *s = yyjson_mut_write(doc, 0, &n);
    yyjson_mut_doc_free(doc);
    if (len != NULL)
        *len = n;
    return s;
}

av_attr_type av_coldef_attr_type(const av_coldef *d, const char *name)
{
    av_schema_attr key;
    memset(&key, 0, sizeof key);
    if (strlen(name) > AV_ATTR_NAME_MAX)
        return AV_AT_TOMBSTONE;
    (void) snprintf(key.name, sizeof key.name, "%s", name);
    const av_schema_attr *r = bsearch(&key, d->attrs, d->n_attrs, sizeof key, attr_cmp);
    return r != NULL ? r->type : AV_AT_TOMBSTONE;
}

bool av_coldef_has_writer(const av_coldef *d, const uint8_t addr[20])
{
    return d->n_writers > 0 && bsearch(addr, d->writers, d->n_writers, 20, addr_cmp) != NULL;
}
