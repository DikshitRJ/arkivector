/*
 * filter/json_filter.c: JSON filter AST parser (see json_filter.h).
 */
#include "filter/json_filter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char buf[128]; /* JSON pointer, truncated if longer */
    size_t len;
} jpath;

typedef struct {
    av_fbuild fb;
    av_err *e;
    jpath path;
} jctx;

/* ------------------------------------------------------------------------- */
/* path and errors                                                           */
/* ------------------------------------------------------------------------- */

static void path_put(jpath *p, char c)
{
    if (p->len + 1 < sizeof(p->buf)) {
        p->buf[p->len++] = c;
        p->buf[p->len] = '\0';
    }
}

/* Append "/key" (RFC 6901 escaped); returns the previous length to restore. */
static size_t path_push(jpath *p, const char *key, size_t n)
{
    size_t old = p->len;
    path_put(p, '/');
    for (size_t i = 0; i < n; i++) {
        if (key[i] == '~') {
            path_put(p, '~');
            path_put(p, '0');
        } else if (key[i] == '/') {
            path_put(p, '~');
            path_put(p, '1');
        } else if ((unsigned char) key[i] < 0x20 || (unsigned char) key[i] >= 0x7f) {
            path_put(p, '?');
        } else {
            path_put(p, key[i]);
        }
    }
    return old;
}

static size_t path_push_idx(jpath *p, size_t idx)
{
    char tmp[24];
    int k = snprintf(tmp, sizeof(tmp), "%zu", idx);
    return path_push(p, tmp, (size_t) k);
}

static void path_pop(jpath *p, size_t old)
{
    p->len = old;
    p->buf[old] = '\0';
}

static void *fail(jctx *c, const char *msg)
{
    av_filter_err(c->e, -1, "%s", msg);
    av_err_field(c->e, c->path.buf);
    return NULL;
}

/* A builder failed: it already set the error; attach the field. */
static av_filter *built(jctx *c, av_filter *n)
{
    if (n == NULL) {
        av_err_field(c->e, c->path.buf);
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* values                                                                    */
/* ------------------------------------------------------------------------- */

/* Convert a JSON scalar to a value of type t. */
static bool jconv(yyjson_val *v, av_attr_type t, av_value *out, const char **why)
{
    if (t == AV_AT_BOOL && yyjson_is_bool(v)) {
        av_value_bool(out, yyjson_get_bool(v));
        return true;
    }
    if (yyjson_is_str(v)) {
        return av_value_parse(t, yyjson_get_str(v), yyjson_get_len(v), out, why);
    }
    if (yyjson_is_int(v) && t != AV_AT_STR && t != AV_AT_BOOL && t != AV_AT_ADDR && t != AV_AT_KEY) {
        char tmp[32];
        int k = yyjson_is_uint(v) ? snprintf(tmp, sizeof(tmp), "%" PRIu64, yyjson_get_uint(v))
                                  : snprintf(tmp, sizeof(tmp), "%" PRId64, yyjson_get_sint(v));
        return av_value_parse(t, tmp, (size_t) k, out, why);
    }
    if (yyjson_is_real(v)) {
        *why = "JSON numbers with a fraction are not accepted; send dec values as strings";
    } else {
        *why = "value has the wrong JSON type for this field";
    }
    return false;
}

static bool jvalue(jctx *c, yyjson_val *v, const av_field *f, av_value *out)
{
    const char *why = "invalid value";
    if (yyjson_is_obj(v)) {
        yyjson_val *ty = yyjson_obj_get(v, "type");
        yyjson_val *va = yyjson_obj_get(v, "value");
        if (yyjson_obj_size(v) != 2 || ty == NULL || va == NULL) {
            fail(c, "a typed value must be an object with exactly the keys \"type\" and \"value\"");
            return false;
        }
        av_attr_type t;
        if (!yyjson_is_str(ty) || !av_type_from_name(yyjson_get_str(ty), yyjson_get_len(ty), &t)) {
            fail(c, "\"type\" must be one of bool, i32, u64, u256, dec, str, addr, key");
            return false;
        }
        if (!av_type_filterable(t)) {
            fail(c, "bytes values cannot be filtered");
            return false;
        }
        if (!jconv(va, t, out, &why)) {
            av_err_set(c->e, "invalid_filter", "invalid %s value: %s", av_type_name(t), why);
            av_err_field(c->e, c->path.buf);
            return false;
        }
        return true;
    }
    av_attr_type t = av_fb_field_type(&c->fb, f);
    if (t == AV_AT_TOMBSTONE) {
        if (yyjson_is_str(v)) {
            t = AV_AT_STR;
        } else if (yyjson_is_bool(v)) {
            t = AV_AT_BOOL;
        } else if (yyjson_is_int(v)) {
            t = AV_AT_I32;
        } else {
            fail(c, "a value must be a typed object, string, boolean or integer");
            return false;
        }
    }
    if (!jconv(v, t, out, &why)) {
        av_err_set(c->e, "invalid_filter", "cannot convert value to %s: %s", av_type_name(t), why);
        av_err_field(c->e, c->path.buf);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* nodes                                                                     */
/* ------------------------------------------------------------------------- */

static av_filter *jnode(jctx *c, yyjson_val *v, int depth);

/* v must be an object with exactly one entry. */
static bool single_entry(jctx *c, yyjson_val *v, yyjson_val **key, yyjson_val **val)
{
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) != 1) {
        fail(c, "expected an object with exactly one key");
        return false;
    }
    yyjson_obj_iter it = yyjson_obj_iter_with(v);
    *key = yyjson_obj_iter_next(&it);
    *val = yyjson_obj_iter_get_val(*key);
    return true;
}

static bool jfield(jctx *c, yyjson_val *key, av_field *f)
{
    if (!av_fb_field(&c->fb, yyjson_get_str(key), yyjson_get_len(key), -1, f, c->e)) {
        av_err_field(c->e, c->path.buf);
        return false;
    }
    return true;
}

static av_filter *j_group(jctx *c, yyjson_val *arr, av_filter_kind kind, int depth)
{
    size_t n = yyjson_is_arr(arr) ? yyjson_arr_size(arr) : 0;
    if (n == 0) {
        return fail(c, "and/or need a non-empty array of filters");
    }
    if (n > AV_FILTER_MAX_LEAVES) {
        return fail(c, "filter has more than 256 predicates");
    }
    av_filter **kids = av_arena_alloc(c->fb.arena, n * sizeof(*kids), 0);
    if (kids == NULL) {
        av_err_set(c->e, "internal", "out of memory building filter");
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        size_t old = path_push_idx(&c->path, i);
        kids[i] = jnode(c, yyjson_arr_get(arr, i), depth + 1);
        path_pop(&c->path, old);
        if (kids[i] == NULL) {
            return NULL;
        }
    }
    return built(c, av_fb_group(&c->fb, kind, -1, kids, n, c->e));
}

static av_filter *jnode(jctx *c, yyjson_val *v, int depth)
{
    if (depth > AV_FILTER_MAX_DEPTH) {
        return fail(c, "filter is nested deeper than 16 levels");
    }
    yyjson_val *key;
    yyjson_val *val;
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) != 1) {
        return fail(c, "a filter node must be an object with exactly one operator key");
    }
    if (!single_entry(c, v, &key, &val)) {
        return NULL;
    }
    const char *op = yyjson_get_str(key);
    size_t opn = yyjson_get_len(key);
    size_t old = path_push(&c->path, op, opn);
    av_filter *res = NULL;
    static const struct {
        const char *name;
        av_cmp_op op;
    } cmps[] = {{"eq", AV_OP_EQ},  {"ne", AV_OP_NE}, {"lt", AV_OP_LT},
                {"lte", AV_OP_LE}, {"gt", AV_OP_GT}, {"gte", AV_OP_GE}};

    if (opn == 3 && memcmp(op, "and", 3) == 0) {
        res = j_group(c, val, AV_F_AND, depth);
    } else if (opn == 2 && memcmp(op, "or", 2) == 0) {
        res = j_group(c, val, AV_F_OR, depth);
    } else if (opn == 3 && memcmp(op, "not", 3) == 0) {
        av_filter *kid = jnode(c, val, depth + 1);
        res = kid == NULL ? NULL : built(c, av_fb_not(&c->fb, -1, kid, c->e));
    } else if (opn == 6 && memcmp(op, "exists", 6) == 0) {
        av_field f;
        if (!yyjson_is_str(val)) {
            fail(c, "exists takes a field name string");
        } else if (av_fb_field(&c->fb, yyjson_get_str(val), yyjson_get_len(val), -1, &f, c->e)) {
            res = built(c, av_fb_exists(&c->fb, -1, &f, c->e));
        } else {
            av_err_field(c->e, c->path.buf);
        }
    } else {
        bool is_cmp = false;
        av_cmp_op cop = AV_OP_EQ;
        for (size_t i = 0; i < sizeof(cmps) / sizeof(cmps[0]); i++) {
            if (strlen(cmps[i].name) == opn && memcmp(cmps[i].name, op, opn) == 0) {
                is_cmp = true;
                cop = cmps[i].op;
            }
        }
        bool is_between = opn == 7 && memcmp(op, "between", 7) == 0;
        bool is_starts = opn == 10 && memcmp(op, "startswith", 10) == 0;
        bool is_typeof = opn == 6 && memcmp(op, "typeof", 6) == 0;
        if (!is_cmp && !is_between && !is_starts && !is_typeof) {
            av_filter_err(c->e, -1, "unknown filter operator '%.*s'", (int) (opn < 40 ? opn : 40), op);
            av_err_field(c->e, c->path.buf);
        } else {
            yyjson_val *fkey;
            yyjson_val *fval;
            if (single_entry(c, val, &fkey, &fval)) {
                av_field f;
                size_t old2 = path_push(&c->path, yyjson_get_str(fkey), yyjson_get_len(fkey));
                if (jfield(c, fkey, &f)) {
                    av_value a;
                    av_value b;
                    if (is_cmp) {
                        if (jvalue(c, fval, &f, &a)) {
                            res = built(c, av_fb_cmp(&c->fb, -1, &f, cop, &a, c->e));
                        }
                    } else if (is_between) {
                        if (!yyjson_is_arr(fval) || yyjson_arr_size(fval) != 2) {
                            fail(c, "between takes an array of exactly [low, high]");
                        } else {
                            size_t o3 = path_push_idx(&c->path, 0);
                            bool ok = jvalue(c, yyjson_arr_get(fval, 0), &f, &a);
                            path_pop(&c->path, o3);
                            if (ok) {
                                o3 = path_push_idx(&c->path, 1);
                                ok = jvalue(c, yyjson_arr_get(fval, 1), &f, &b);
                                path_pop(&c->path, o3);
                            }
                            if (ok) {
                                res = built(c, av_fb_between(&c->fb, -1, &f, &a, &b, c->e));
                            }
                        }
                    } else if (is_starts) {
                        if (!yyjson_is_str(fval)) {
                            fail(c, "startswith takes a string prefix");
                        } else if (!av_value_str(&a, (const uint8_t *) yyjson_get_str(fval),
                                                 yyjson_get_len(fval))) {
                            fail(c, "prefix must be valid UTF-8 of at most 128 bytes");
                        } else {
                            res = built(c, av_fb_startswith(&c->fb, -1, &f, &a, c->e));
                        }
                    } else {
                        av_attr_type t;
                        if (!yyjson_is_str(fval) ||
                            !av_type_from_name(yyjson_get_str(fval), yyjson_get_len(fval), &t)) {
                            fail(c, "typeof takes a type name (bool, i32, u64, u256, dec, str, addr, key, "
                                    "bytes)");
                        } else {
                            res = built(c, av_fb_typeof(&c->fb, -1, &f, t, c->e));
                        }
                    }
                }
                path_pop(&c->path, old2);
            }
        }
    }
    path_pop(&c->path, old);
    return res;
}

av_status av_filter_from_json(yyjson_val *v, const av_coldef *sch, av_arena *a, av_filter **out, av_err *e)
{
    if (out != NULL) {
        *out = NULL;
    }
    av_err_clear(e);
    if (out == NULL || a == NULL || v == NULL) {
        av_err_set(e, "internal", "av_filter_from_json: bad arguments");
        return AV_ERR;
    }
    jctx c;
    memset(&c, 0, sizeof(c));
    av_fb_init(&c.fb, a, sch);
    c.e = e;
    av_filter *f = jnode(&c, v, 1);
    if (f == NULL) {
        return AV_ERR;
    }
    *out = f;
    return AV_OK;
}
