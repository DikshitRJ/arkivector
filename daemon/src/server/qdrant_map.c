/*
 * server/qdrant_map.c: Qdrant REST shapes <-> Arkivector entities, filters and payloads.
 * See qdrant_map.h.
 */
#include "server/qdrant_map.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"

#define QD_MAX_POINTS 10000
#define QD_MAX_DEPTH  8

/* ------------------------------------------------------------------ ids */

static bool is_hex(char c)
{
    return isxdigit((unsigned char) c) != 0;
}

av_status av_qdrant_point_id(yyjson_val *id, char out[AV_QDRANT_ID_MAX], av_err *e)
{
    if (yyjson_is_uint(id) || (yyjson_is_sint(id) && yyjson_get_sint(id) >= 0)) {
        snprintf(out, AV_QDRANT_ID_MAX, "%" PRIu64, (uint64_t) yyjson_get_uint(id));
        return AV_OK;
    }
    const char *s = yyjson_get_str(id);
    if (s == NULL) {
        av_err_set(e, "invalid_argument", "point id must be an unsigned integer or a UUID");
        return AV_ERR;
    }
    size_t n = yyjson_get_len(id);
    char hex[32];
    size_t h = 0;
    if (n == 36) {
        for (size_t i = 0; i < 36; i++) {
            bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? s[i] != '-' : !is_hex(s[i]))
                goto bad;
            if (!dash)
                hex[h++] = (char) tolower((unsigned char) s[i]);
        }
    } else if (n == 32) {
        for (size_t i = 0; i < 32; i++) {
            if (!is_hex(s[i]))
                goto bad;
            hex[h++] = (char) tolower((unsigned char) s[i]);
        }
    } else {
        goto bad;
    }
    snprintf(out, AV_QDRANT_ID_MAX, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20);
    return AV_OK;
bad:
    av_err_set(e, "invalid_argument", "point id must be an unsigned integer or a UUID");
    return AV_ERR;
}

yyjson_mut_val *av_qdrant_id_json(yyjson_mut_doc *doc, const char *id)
{
    size_t n = strlen(id);
    bool digits = n > 0 && n <= 20 && (n == 1 || id[0] != '0');
    for (size_t i = 0; digits && i < n; i++)
        digits = id[i] >= '0' && id[i] <= '9';
    if (digits) {
        errno = 0;
        char *end = NULL;
        unsigned long long v = strtoull(id, &end, 10);
        if (errno == 0 && end != NULL && *end == '\0')
            return yyjson_mut_uint(doc, (uint64_t) v);
    }
    return yyjson_mut_strcpy(doc, id);
}

/* ------------------------------------------------------------------ typed values */

/* A JSON double as a plain decimal string (no exponent), for the dec type. */
static bool dec_string(double d, char *buf, size_t cap)
{
    if (!isfinite(d))
        return false;
    int n = snprintf(buf, cap, "%.17g", d);
    if (n < 0 || (size_t) n >= cap)
        return false;
    if (strchr(buf, 'e') == NULL && strchr(buf, 'E') == NULL)
        return true;
    if (fabs(d) >= 1e30)
        return false; /* far beyond the dec range anyway */
    n = snprintf(buf, cap, "%.18f", d);
    if (n < 0 || (size_t) n >= cap)
        return false;
    char *dot = strchr(buf, '.');
    if (dot != NULL) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0')
            *end-- = '\0';
        if (end == dot)
            *end = '\0';
    }
    return true;
}

static yyjson_mut_val *typed(yyjson_mut_doc *doc, const char *type, yyjson_mut_val *value)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, o, "type", type);
    yyjson_mut_obj_add_val(doc, o, "value", value);
    return o;
}

/* Scalar JSON -> TypedValue with the upsert typing rules. NULL when v is not a usable scalar. */
static yyjson_mut_val *typed_scalar(yyjson_mut_doc *doc, yyjson_val *v)
{
    char buf[64];
    if (yyjson_is_bool(v))
        return typed(doc, "bool", yyjson_mut_bool(doc, yyjson_get_bool(v)));
    if (yyjson_is_str(v))
        return typed(doc, "str", yyjson_mut_strncpy(doc, yyjson_get_str(v), yyjson_get_len(v)));
    if (yyjson_is_sint(v)) {
        int64_t x = yyjson_get_sint(v);
        if (x >= INT32_MIN && x <= INT32_MAX)
            return typed(doc, "i32", yyjson_mut_sint(doc, x));
        snprintf(buf, sizeof buf, "%" PRId64, x);
        return typed(doc, x >= 0 ? "u64" : "dec", yyjson_mut_strcpy(doc, buf));
    }
    if (yyjson_is_uint(v)) {
        uint64_t x = yyjson_get_uint(v);
        if (x <= INT32_MAX)
            return typed(doc, "i32", yyjson_mut_sint(doc, (int64_t) x));
        snprintf(buf, sizeof buf, "%" PRIu64, x);
        return typed(doc, "u64", yyjson_mut_strcpy(doc, buf));
    }
    if (yyjson_is_real(v)) {
        if (!dec_string(yyjson_get_real(v), buf, sizeof buf))
            return NULL;
        return typed(doc, "dec", yyjson_mut_strcpy(doc, buf));
    }
    return NULL;
}

/* Arkiv attribute name: [A-Za-z][A-Za-z0-9._-]{0,31}, not in the reserved av./arkiv. spaces. */
static bool attr_name_ok(const char *k, size_t n)
{
    if (n == 0 || n > AV_ATTR_NAME_MAX || !isalpha((unsigned char) k[0]))
        return false;
    for (size_t i = 1; i < n; i++) {
        unsigned char c = (unsigned char) k[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-')
            return false;
    }
    return !(n >= 3 && strncmp(k, "av.", 3) == 0) && !(n >= 6 && strncmp(k, "arkiv.", 6) == 0);
}

/* ------------------------------------------------------------------ points */

static av_status one_point(yyjson_val *id, yyjson_val *vector, yyjson_val *payload, yyjson_mut_doc *doc,
                           yyjson_mut_val *arr, size_t *n_dropped, av_err *e)
{
    char idbuf[AV_QDRANT_ID_MAX];
    if (av_qdrant_point_id(id, idbuf, e) != AV_OK)
        return AV_ERR;
    if (vector == NULL || yyjson_is_null(vector)) {
        av_err_set(e, "invalid_argument", "point %s has no vector", idbuf);
        return AV_ERR;
    }
    if (!yyjson_is_arr(vector)) {
        av_err_set(e, "not_implemented", "named and sparse vectors are not supported (point %s)", idbuf);
        return AV_ERR;
    }
    size_t i, max;
    yyjson_val *x;
    yyjson_arr_foreach(vector, i, max, x)
    {
        if (!yyjson_is_num(x)) {
            av_err_set(e, "invalid_vector", "point %s: vector elements must be numbers", idbuf);
            return AV_ERR;
        }
    }
    if (payload != NULL && !yyjson_is_null(payload) && !yyjson_is_obj(payload)) {
        av_err_set(e, "invalid_argument", "point %s: payload must be an object", idbuf);
        return AV_ERR;
    }

    yyjson_mut_val *ent = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, ent, "id", idbuf);
    yyjson_mut_obj_add_val(doc, ent, "vector", yyjson_val_mut_copy(doc, vector));

    if (yyjson_is_obj(payload)) {
        yyjson_val *text = yyjson_obj_get(payload, "page_content");
        const char *text_key = "page_content";
        if (!yyjson_is_str(text)) {
            text = yyjson_obj_get(payload, "text");
            text_key = "text";
        }
        if (!yyjson_is_str(text)) {
            text = NULL;
            text_key = NULL;
        }
        yyjson_mut_val *attrs = yyjson_mut_obj(doc);
        yyjson_mut_val *extras = yyjson_mut_obj(doc);
        size_t n_extras = 0;
        yyjson_obj_iter it;
        yyjson_obj_iter_init(payload, &it);
        yyjson_val *k;
        while ((k = yyjson_obj_iter_next(&it)) != NULL) {
            yyjson_val *v = yyjson_obj_iter_get_val(k);
            const char *ks = yyjson_get_str(k);
            size_t kn = yyjson_get_len(k);
            if (text_key != NULL && strcmp(ks, text_key) == 0)
                continue;
            if (yyjson_is_null(v))
                continue;
            yyjson_mut_val *tv = NULL;
            bool keepable =
                attr_name_ok(ks, kn) && !(yyjson_is_str(v) && yyjson_get_len(v) > AV_ATTR_STR_MAX);
            if (keepable)
                tv = typed_scalar(doc, v);
            if (tv != NULL) {
                yyjson_mut_obj_add(attrs, yyjson_mut_strncpy(doc, ks, kn), tv);
            } else {
                yyjson_mut_obj_add(extras, yyjson_mut_strncpy(doc, ks, kn), yyjson_val_mut_copy(doc, v));
                n_extras++;
            }
        }
        if (text != NULL) {
            yyjson_mut_obj_add_strncpy(doc, ent, "document", yyjson_get_str(text), yyjson_get_len(text));
            *n_dropped += n_extras;
        } else if (n_extras > 0) {
            size_t len = 0;
            char *js = yyjson_mut_val_write(extras, 0, &len);
            if (js == NULL) {
                av_err_set(e, "internal", "out of memory");
                return AV_ERR;
            }
            yyjson_mut_obj_add_strncpy(doc, ent, "document", js, len);
            free(js);
        }
        if (yyjson_mut_obj_size(attrs) > 0)
            yyjson_mut_obj_add_val(doc, ent, "attributes", attrs);
    }
    yyjson_mut_arr_add_val(arr, ent);
    return AV_OK;
}

av_status av_qdrant_points_to_entities(yyjson_val *body, yyjson_mut_doc *doc, yyjson_mut_val **out_arr,
                                       size_t *n_dropped, av_err *e)
{
    *out_arr = NULL;
    *n_dropped = 0;
    if (!yyjson_is_obj(body)) {
        av_err_set(e, "bad_request", "request body must be an object");
        return AV_ERR;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_val *points = yyjson_obj_get(body, "points");
    yyjson_val *batch = yyjson_obj_get(body, "batch");
    if (points != NULL) {
        if (!yyjson_is_arr(points) || yyjson_arr_size(points) > QD_MAX_POINTS) {
            av_err_set(e, "invalid_argument", "points must be an array of at most %d points", QD_MAX_POINTS);
            return AV_ERR;
        }
        size_t i, max;
        yyjson_val *p;
        yyjson_arr_foreach(points, i, max, p)
        {
            if (!yyjson_is_obj(p)) {
                av_err_set(e, "invalid_argument", "points[%zu] must be an object", i);
                return AV_ERR;
            }
            if (one_point(yyjson_obj_get(p, "id"), yyjson_obj_get(p, "vector"), yyjson_obj_get(p, "payload"),
                          doc, arr, n_dropped, e) != AV_OK)
                return AV_ERR;
        }
    } else if (yyjson_is_obj(batch)) {
        yyjson_val *ids = yyjson_obj_get(batch, "ids");
        yyjson_val *vecs = yyjson_obj_get(batch, "vectors");
        yyjson_val *pls = yyjson_obj_get(batch, "payloads");
        size_t n = yyjson_arr_size(ids);
        if (!yyjson_is_arr(ids) || !yyjson_is_arr(vecs) || yyjson_arr_size(vecs) != n || n > QD_MAX_POINTS ||
            (pls != NULL && !yyjson_is_null(pls) && (!yyjson_is_arr(pls) || yyjson_arr_size(pls) != n))) {
            av_err_set(e, "invalid_argument",
                       "batch needs ids and vectors (and payloads) of the same length");
            return AV_ERR;
        }
        for (size_t i = 0; i < n; i++) {
            yyjson_val *pl = yyjson_is_arr(pls) ? yyjson_arr_get(pls, i) : NULL;
            if (one_point(yyjson_arr_get(ids, i), yyjson_arr_get(vecs, i), pl, doc, arr, n_dropped, e) !=
                AV_OK)
                return AV_ERR;
        }
    } else {
        av_err_set(e, "invalid_argument", "body needs \"points\" or \"batch\"");
        return AV_ERR;
    }
    *out_arr = arr;
    return AV_OK;
}

/* ------------------------------------------------------------------ filters */

static yyjson_mut_val *op1(yyjson_mut_doc *doc, const char *op, const char *key, yyjson_mut_val *v)
{
    yyjson_mut_val *inner = yyjson_mut_obj(doc);
    yyjson_mut_obj_add(inner, yyjson_mut_strcpy(doc, key), v);
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, o, op, inner);
    return o;
}

static yyjson_mut_val *group(yyjson_mut_doc *doc, const char *op, yyjson_mut_val *arr)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, o, op, arr);
    return o;
}

static yyjson_mut_val *negate(yyjson_mut_doc *doc, yyjson_mut_val *f)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, o, "not", f);
    return o;
}

static av_status filter_rec(yyjson_val *qf, yyjson_mut_doc *doc, int depth, yyjson_mut_val **out, av_err *e);

static av_status match_cond(const char *key, yyjson_val *match, yyjson_mut_doc *doc, yyjson_mut_val **out,
                            av_err *e)
{
    yyjson_val *value = yyjson_obj_get(match, "value");
    yyjson_val *any = yyjson_obj_get(match, "any");
    yyjson_val *except = yyjson_obj_get(match, "except");
    if (value != NULL) {
        yyjson_mut_val *tv = typed_scalar(doc, value);
        if (tv == NULL) {
            av_err_set(e, "invalid_filter",
                       "match.value for '%s' must be a string, integer, float or boolean", key);
            return AV_ERR;
        }
        *out = op1(doc, "eq", key, tv);
        return AV_OK;
    }
    yyjson_val *list = any != NULL ? any : except;
    if (list != NULL) {
        if (!yyjson_is_arr(list) || yyjson_arr_size(list) == 0 || yyjson_arr_size(list) > 256) {
            av_err_set(e, "invalid_filter", "match.%s for '%s' must be a non-empty array",
                       any ? "any" : "except", key);
            return AV_ERR;
        }
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        size_t i, max;
        yyjson_val *v;
        yyjson_arr_foreach(list, i, max, v)
        {
            yyjson_mut_val *tv = typed_scalar(doc, v);
            if (tv == NULL) {
                av_err_set(e, "invalid_filter", "match values for '%s' must be scalars", key);
                return AV_ERR;
            }
            yyjson_mut_arr_add_val(arr, op1(doc, "eq", key, tv));
        }
        yyjson_mut_val *o = group(doc, "or", arr);
        *out = any != NULL ? o : negate(doc, o);
        return AV_OK;
    }
    av_err_set(e, "not_implemented", "match on '%s': only value, any and except are supported", key);
    return AV_ERR;
}

/* range {gt,gte,lt,lte}: one AND per numeric type, OR'ed (ints and floats are stored as different types). */
static av_status range_cond(const char *key, yyjson_val *range, yyjson_mut_doc *doc, yyjson_mut_val **out,
                            av_err *e)
{
    static const char *const ops[] = {"gt", "gte", "lt", "lte"};
    static const char *const types[] = {"i32", "u64", "dec"};
    yyjson_mut_val *branches = yyjson_mut_arr(doc);
    size_t n_bounds = 0;
    for (size_t t = 0; t < 3; t++) {
        yyjson_mut_val *conj = yyjson_mut_arr(doc);
        bool possible = true;
        for (size_t o = 0; o < 4 && possible; o++) {
            yyjson_val *b = yyjson_obj_get(range, ops[o]);
            if (b == NULL || yyjson_is_null(b))
                continue;
            if (!yyjson_is_num(b)) {
                av_err_set(e, "invalid_filter", "range.%s for '%s' must be a number", ops[o], key);
                return AV_ERR;
            }
            if (t == 0)
                n_bounds++;
            double d = yyjson_get_num(b);
            const char *op = ops[o];
            char buf[64];
            yyjson_mut_val *val = NULL;
            if (t == 2) {
                if (!dec_string(d, buf, sizeof buf)) {
                    av_err_set(e, "invalid_filter", "range bound for '%s' is out of range", key);
                    return AV_ERR;
                }
                val = yyjson_mut_strcpy(doc, buf);
            } else {
                /* integer types: a fractional bound tightens to the next integer */
                double r = d;
                if (floor(d) != d) {
                    if (o <= 1) {
                        r = ceil(d);
                        op = "gte";
                    } else {
                        r = floor(d);
                        op = "lte";
                    }
                }
                double lo = t == 0 ? (double) INT32_MIN : 0.0;
                double hi = t == 0 ? (double) INT32_MAX : 18446744073709551615.0;
                if (r < lo) {
                    if (o >= 2)
                        possible = false; /* x < lo: no value of this type */
                    continue;             /* x > lo: always true for this type */
                }
                if (r > hi) {
                    if (o <= 1)
                        possible = false;
                    continue;
                }
                if (t == 0)
                    val = yyjson_mut_sint(doc, (int64_t) r);
                else {
                    snprintf(buf, sizeof buf, "%.0f", r);
                    val = yyjson_mut_strcpy(doc, buf);
                }
            }
            yyjson_mut_arr_add_val(conj, op1(doc, op, key, typed(doc, types[t], val)));
        }
        if (!possible)
            continue;
        if (yyjson_mut_arr_size(conj) == 0) {
            /* every bound was trivially true for this type: match any value of the type */
            yyjson_mut_val *tobj = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, tobj, key, types[t]);
            yyjson_mut_val *o = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, o, "typeof", tobj);
            yyjson_mut_arr_add_val(branches, o);
        } else {
            yyjson_mut_arr_add_val(branches, group(doc, "and", conj));
        }
    }
    if (n_bounds == 0) {
        av_err_set(e, "invalid_filter", "range for '%s' needs gt, gte, lt or lte", key);
        return AV_ERR;
    }
    if (yyjson_mut_arr_size(branches) == 0) {
        /* impossible for every numeric type: exists(key) AND NOT exists(key) matches nothing */
        yyjson_mut_val *ex1 = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, ex1, "exists", key);
        yyjson_mut_val *ex2 = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, ex2, "exists", key);
        yyjson_mut_val *both = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_val(both, ex1);
        yyjson_mut_arr_add_val(both, negate(doc, ex2));
        *out = group(doc, "and", both);
        return AV_OK;
    }
    *out = group(doc, "or", branches);
    return AV_OK;
}

static av_status condition(yyjson_val *c, yyjson_mut_doc *doc, int depth, yyjson_mut_val **out, av_err *e)
{
    if (!yyjson_is_obj(c)) {
        av_err_set(e, "invalid_filter", "filter conditions must be objects");
        return AV_ERR;
    }
    if (yyjson_obj_get(c, "must") != NULL || yyjson_obj_get(c, "should") != NULL ||
        yyjson_obj_get(c, "must_not") != NULL)
        return filter_rec(c, doc, depth + 1, out, e);

    yyjson_val *has_id = yyjson_obj_get(c, "has_id");
    if (has_id != NULL) {
        if (!yyjson_is_arr(has_id) || yyjson_arr_size(has_id) == 0 || yyjson_arr_size(has_id) > 256) {
            av_err_set(e, "invalid_filter", "has_id must be a non-empty array of ids");
            return AV_ERR;
        }
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        size_t i, max;
        yyjson_val *v;
        yyjson_arr_foreach(has_id, i, max, v)
        {
            char idbuf[AV_QDRANT_ID_MAX];
            if (av_qdrant_point_id(v, idbuf, e) != AV_OK)
                return AV_ERR;
            yyjson_mut_arr_add_val(arr,
                                   op1(doc, "eq", "av.id", typed(doc, "str", yyjson_mut_strcpy(doc, idbuf))));
        }
        *out = group(doc, "or", arr);
        return AV_OK;
    }
    yyjson_val *empty = yyjson_obj_get(c, "is_empty");
    if (empty == NULL)
        empty = yyjson_obj_get(c, "is_null");
    if (empty != NULL) {
        const char *k = yyjson_get_str(yyjson_obj_get(empty, "key"));
        if (k == NULL) {
            av_err_set(e, "invalid_filter", "is_empty / is_null need a key");
            return AV_ERR;
        }
        if (strchr(k, '.') != NULL) {
            av_err_set(e, "not_implemented", "payload key paths with dots are not supported ('%s')", k);
            return AV_ERR;
        }
        yyjson_mut_val *ex = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, ex, "exists", k);
        *out = negate(doc, ex);
        return AV_OK;
    }
    const char *key = yyjson_get_str(yyjson_obj_get(c, "key"));
    if (key == NULL) {
        av_err_set(e, "not_implemented", "unsupported filter condition (nested, geo, values_count, ...)");
        return AV_ERR;
    }
    if (strchr(key, '.') != NULL || key[0] == '\0') {
        av_err_set(e, "not_implemented", "payload key paths with dots are not supported ('%s')", key);
        return AV_ERR;
    }
    yyjson_val *match = yyjson_obj_get(c, "match");
    yyjson_val *range = yyjson_obj_get(c, "range");
    if (yyjson_is_obj(match))
        return match_cond(key, match, doc, out, e);
    if (yyjson_is_obj(range))
        return range_cond(key, range, doc, out, e);
    av_err_set(e, "not_implemented", "condition on '%s': only match and range are supported", key);
    return AV_ERR;
}

static av_status clause(yyjson_val *qf, const char *name, yyjson_mut_doc *doc, int depth,
                        yyjson_mut_val **out, av_err *e)
{
    *out = NULL;
    yyjson_val *v = yyjson_obj_get(qf, name);
    if (v == NULL || yyjson_is_null(v))
        return AV_OK;
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (yyjson_is_obj(v)) { /* Qdrant accepts a single condition too */
        yyjson_mut_val *c = NULL;
        if (condition(v, doc, depth, &c, e) != AV_OK)
            return AV_ERR;
        yyjson_mut_arr_add_val(arr, c);
    } else if (yyjson_is_arr(v)) {
        if (yyjson_arr_size(v) > 256) {
            av_err_set(e, "invalid_filter", "too many conditions in %s", name);
            return AV_ERR;
        }
        size_t i, max;
        yyjson_val *c;
        yyjson_arr_foreach(v, i, max, c)
        {
            yyjson_mut_val *m = NULL;
            if (condition(c, doc, depth, &m, e) != AV_OK)
                return AV_ERR;
            yyjson_mut_arr_add_val(arr, m);
        }
    } else {
        av_err_set(e, "invalid_filter", "%s must be an array of conditions", name);
        return AV_ERR;
    }
    if (yyjson_mut_arr_size(arr) > 0)
        *out = arr;
    return AV_OK;
}

static av_status filter_rec(yyjson_val *qf, yyjson_mut_doc *doc, int depth, yyjson_mut_val **out, av_err *e)
{
    *out = NULL;
    if (depth > QD_MAX_DEPTH) {
        av_err_set(e, "invalid_filter", "filter nesting deeper than %d", QD_MAX_DEPTH);
        return AV_ERR;
    }
    if (!yyjson_is_obj(qf)) {
        av_err_set(e, "invalid_filter", "filter must be an object");
        return AV_ERR;
    }
    if (yyjson_obj_get(qf, "min_should") != NULL) {
        av_err_set(e, "not_implemented", "min_should is not supported");
        return AV_ERR;
    }
    yyjson_mut_val *must = NULL, *should = NULL, *must_not = NULL;
    if (clause(qf, "must", doc, depth, &must, e) != AV_OK ||
        clause(qf, "should", doc, depth, &should, e) != AV_OK ||
        clause(qf, "must_not", doc, depth, &must_not, e) != AV_OK)
        return AV_ERR;
    yyjson_mut_val *all = yyjson_mut_arr(doc);
    if (must != NULL)
        yyjson_mut_arr_add_val(all, group(doc, "and", must));
    if (should != NULL)
        yyjson_mut_arr_add_val(all, group(doc, "or", should));
    if (must_not != NULL)
        yyjson_mut_arr_add_val(all, negate(doc, group(doc, "or", must_not)));
    size_t n = yyjson_mut_arr_size(all);
    if (n == 0)
        return AV_OK;
    *out = n == 1 ? yyjson_mut_arr_get_first(all) : group(doc, "and", all);
    return AV_OK;
}

av_status av_qdrant_filter_to_json(yyjson_val *qf, yyjson_mut_doc *doc, yyjson_mut_val **out, av_err *e)
{
    *out = NULL;
    if (qf == NULL || yyjson_is_null(qf))
        return AV_OK;
    return filter_rec(qf, doc, 0, out, e);
}

/* ------------------------------------------------------------------ payloads */

bool av_qdrant_wants_payload(yyjson_val *wp)
{
    if (wp == NULL || yyjson_is_null(wp))
        return false;
    if (yyjson_is_bool(wp))
        return yyjson_get_bool(wp);
    return yyjson_is_arr(wp) || yyjson_is_obj(wp);
}

static bool key_selected(yyjson_val *wp, const char *key)
{
    if (wp == NULL || yyjson_is_bool(wp))
        return true;
    yyjson_val *list = wp;
    bool include = true;
    if (yyjson_is_obj(wp)) {
        list = yyjson_obj_get(wp, "include");
        if (list == NULL) {
            list = yyjson_obj_get(wp, "exclude");
            include = false;
        }
    }
    if (!yyjson_is_arr(list))
        return true;
    bool found = false;
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(list, i, max, v)
    {
        const char *s = yyjson_get_str(v);
        if (s != NULL && strcmp(s, key) == 0)
            found = true;
    }
    return include ? found : !found;
}

static yyjson_mut_val *plain_value(yyjson_mut_doc *doc, yyjson_mut_val *tv)
{
    const char *type = yyjson_mut_get_str(yyjson_mut_obj_get(tv, "type"));
    yyjson_mut_val *v = yyjson_mut_obj_get(tv, "value");
    if (type == NULL || v == NULL)
        return yyjson_mut_null(doc);
    const char *s = yyjson_mut_get_str(v);
    if (s != NULL && strcmp(type, "u64") == 0) {
        char *end = NULL;
        errno = 0;
        unsigned long long x = strtoull(s, &end, 10);
        if (errno == 0 && end != s && *end == '\0')
            return yyjson_mut_uint(doc, (uint64_t) x);
    }
    if (s != NULL && strcmp(type, "dec") == 0) {
        char *end = NULL;
        double d = strtod(s, &end);
        if (end != s && *end == '\0' && isfinite(d))
            return yyjson_mut_real(doc, d); /* floats were stored as dec */
    }
    if (s != NULL)
        return yyjson_mut_strcpy(doc, s);
    if (yyjson_mut_is_bool(v))
        return yyjson_mut_bool(doc, yyjson_mut_get_bool(v));
    if (yyjson_mut_is_sint(v))
        return yyjson_mut_sint(doc, yyjson_mut_get_sint(v));
    if (yyjson_mut_is_uint(v))
        return yyjson_mut_uint(doc, yyjson_mut_get_uint(v));
    return yyjson_mut_null(doc);
}

yyjson_mut_val *av_qdrant_payload(yyjson_mut_doc *doc, yyjson_mut_val *attrs_obj, const char *document,
                                  size_t doc_len, yyjson_val *wp)
{
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    if (yyjson_mut_is_obj(attrs_obj)) {
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(attrs_obj, &it);
        yyjson_mut_val *k;
        while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
            const char *ks = yyjson_mut_get_str(k);
            if (ks == NULL || !key_selected(wp, ks))
                continue;
            yyjson_mut_obj_add(out, yyjson_mut_strcpy(doc, ks),
                               plain_value(doc, yyjson_mut_obj_iter_get_val(k)));
        }
    }
    if (document != NULL) {
        bool merged = false;
        if (doc_len > 1 && document[0] == '{') {
            yyjson_doc *jd = yyjson_read(document, doc_len, 0);
            yyjson_val *root = jd != NULL ? yyjson_doc_get_root(jd) : NULL;
            if (yyjson_is_obj(root)) {
                yyjson_obj_iter it;
                yyjson_obj_iter_init(root, &it);
                yyjson_val *k;
                while ((k = yyjson_obj_iter_next(&it)) != NULL) {
                    const char *ks = yyjson_get_str(k);
                    if (!key_selected(wp, ks) || yyjson_mut_obj_get(out, ks) != NULL)
                        continue;
                    yyjson_mut_obj_add(out, yyjson_mut_strcpy(doc, ks),
                                       yyjson_val_mut_copy(doc, yyjson_obj_iter_get_val(k)));
                }
                merged = true;
            }
            if (jd != NULL)
                yyjson_doc_free(jd);
        }
        if (!merged && key_selected(wp, "page_content"))
            yyjson_mut_obj_add_strncpy(doc, out, "page_content", document, doc_len);
    }
    return out;
}
