/*
 * filter/ast.c: AST constructors with validation, equality and printing (see ast.h).
 */
#include "filter/ast.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* errors                                                                    */
/* ------------------------------------------------------------------------- */

void av_filter_err(av_err *e, int pos, const char *fmt, ...)
{
    if (e == NULL) {
        return;
    }
    char msg[sizeof(e->msg)];
    va_list ap;
    va_start(ap, fmt);
    (void) vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    av_err_set(e, "invalid_filter", "%s", msg);
    e->position = pos >= 0 ? pos : -1;
}

static void err_internal(av_err *e)
{
    av_err_set(e, "internal", "out of memory building filter");
}

/* ------------------------------------------------------------------------- */
/* names and fields                                                          */
/* ------------------------------------------------------------------------- */

static const char *const k_reserved[] = {"and",  "or",     "not",    "true",   "false", "between", "startswith",
                                         "exists", "typeof", "order",  "by",     "asc",   "desc",    "select",
                                         "from", "where",  "limit",  "offset", "like",  "is",      "null",
                                         "bool", "i32",    "u64",    "u256",   "dec",   "str",     "addr",
                                         "key",  "bytes"};

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool av_filter_valid_attr_name(const char *name, size_t n, const char **why)
{
    const char *dummy;
    if (why == NULL) {
        why = &dummy;
    }
    if (n == 0 || n > AV_ATTR_NAME_MAX) {
        *why = "attribute name must be 1 to 32 bytes";
        return false;
    }
    if (!is_alpha(name[0])) {
        *why = "attribute name must start with an ASCII letter";
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!(is_alpha(c) || is_digit(c) || c == '.' || c == '-' || c == '_')) {
            *why = "attribute name may only contain letters, digits, '.', '-' and '_'";
            return false;
        }
        if (c == '-' && i + 1 < n && name[i + 1] == '-') {
            *why = "attribute name cannot contain '--'";
            return false;
        }
    }
    char low[AV_ATTR_NAME_MAX + 1];
    for (size_t i = 0; i < n; i++) {
        low[i] = (char) ((name[i] >= 'A' && name[i] <= 'Z') ? name[i] - 'A' + 'a' : name[i]);
    }
    low[n] = '\0';
    for (size_t i = 0; i < sizeof(k_reserved) / sizeof(k_reserved[0]); i++) {
        if (strcmp(low, k_reserved[i]) == 0) {
            *why = "attribute name is a reserved word";
            return false;
        }
    }
    return true;
}

static av_attr_type system_type(av_field_kind k)
{
    switch (k) {
    case AV_FIELD_KEY:
        return AV_AT_KEY;
    case AV_FIELD_OWNER:
    case AV_FIELD_CREATOR:
        return AV_AT_ADDR;
    case AV_FIELD_EXPIRES:
        return AV_AT_U64;
    default:
        return AV_AT_TOMBSTONE;
    }
}

static av_attr_type schema_type(const av_coldef *sch, const char *name)
{
    if (sch == NULL || sch->attrs == NULL) {
        return AV_AT_TOMBSTONE;
    }
    for (size_t i = 0; i < sch->n_attrs; i++) {
        if (strcmp(sch->attrs[i].name, name) == 0) {
            return sch->attrs[i].type;
        }
    }
    return AV_AT_TOMBSTONE;
}

void av_fb_init(av_fbuild *fb, av_arena *a, const av_coldef *schema)
{
    fb->arena = a;
    fb->schema = schema;
    fb->leaves = 0;
}

bool av_fb_field(av_fbuild *fb, const char *name, size_t n, int pos, av_field *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (n > 0 && name[0] == '$') {
        static const struct {
            const char *s;
            av_field_kind k;
        } sys[] = {{"$key", AV_FIELD_KEY},
                   {"$owner", AV_FIELD_OWNER},
                   {"$creator", AV_FIELD_CREATOR},
                   {"$expiresAt", AV_FIELD_EXPIRES}};
        for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++) {
            if (strlen(sys[i].s) == n && memcmp(sys[i].s, name, n) == 0) {
                out->kind = sys[i].k;
                memcpy(out->name, name, n);
                return true;
            }
        }
        if (n == 10 && (memcmp(name, "$createdAt", 10) == 0 || memcmp(name, "$updatedAt", 10) == 0)) {
            av_filter_err(e, pos, "system field %.10s cannot be used in a filter", name);
        } else if (n == 14 && memcmp(name, "$creationFlags", 14) == 0) {
            av_filter_err(e, pos, "system field $creationFlags cannot be used in a filter");
        } else {
            av_filter_err(e, pos, "unknown system field (expected $key, $owner, $creator or $expiresAt)");
        }
        return false;
    }
    const char *why = NULL;
    if (!av_filter_valid_attr_name(name, n, &why)) {
        av_filter_err(e, pos, "invalid attribute name: %s", why);
        return false;
    }
    out->kind = AV_FIELD_ATTR;
    memcpy(out->name, name, n);
    if (schema_type(fb->schema, out->name) == AV_AT_BYTES) {
        av_filter_err(e, pos, "attribute '%s' is bytes and cannot be filtered", out->name);
        return false;
    }
    return true;
}

av_attr_type av_fb_field_type(const av_fbuild *fb, const av_field *f)
{
    if (f->kind != AV_FIELD_ATTR) {
        return system_type(f->kind);
    }
    return schema_type(fb->schema, f->name);
}

/* ------------------------------------------------------------------------- */
/* constructors                                                              */
/* ------------------------------------------------------------------------- */

static av_filter *node_new(av_fbuild *fb, av_filter_kind kind, int pos, av_err *e)
{
    av_filter *f = av_arena_calloc(fb->arena, sizeof(*f), 0);
    if (f == NULL) {
        err_internal(e);
        return NULL;
    }
    f->kind = kind;
    f->pos = pos >= 0 ? pos : -1;
    f->depth = 1;
    return f;
}

static bool leaf_count(av_fbuild *fb, int pos, av_err *e)
{
    if (fb->leaves >= AV_FILTER_MAX_LEAVES) {
        av_filter_err(e, pos, "filter has more than %d predicates", AV_FILTER_MAX_LEAVES);
        return false;
    }
    fb->leaves++;
    return true;
}

static bool check_value(const av_field *f, const av_value *v, int pos, av_err *e)
{
    if (!av_type_filterable(v->type)) {
        av_filter_err(e, pos, "bytes values cannot be filtered");
        return false;
    }
    if (f->kind != AV_FIELD_ATTR && system_type(f->kind) != v->type) {
        av_filter_err(e, pos, "%s is of type %s, not %s", f->name, av_type_name(system_type(f->kind)),
                      av_type_name(v->type));
        return false;
    }
    return true;
}

av_filter *av_fb_cmp(av_fbuild *fb, int pos, const av_field *f, av_cmp_op op, const av_value *v, av_err *e)
{
    if (!check_value(f, v, pos, e) || !leaf_count(fb, pos, e)) {
        return NULL;
    }
    av_filter *n = node_new(fb, AV_F_CMP, pos, e);
    if (n != NULL) {
        n->field = *f;
        n->op = op;
        n->value = *v;
    }
    return n;
}

av_filter *av_fb_between(av_fbuild *fb, int pos, const av_field *f, const av_value *lo, const av_value *hi,
                         av_err *e)
{
    if (!check_value(f, lo, pos, e) || !check_value(f, hi, pos, e)) {
        return NULL;
    }
    if (lo->type != hi->type) {
        av_filter_err(e, pos, "BETWEEN bounds must have the same type (%s and %s)", av_type_name(lo->type),
                      av_type_name(hi->type));
        return NULL;
    }
    if (!leaf_count(fb, pos, e)) {
        return NULL;
    }
    av_filter *n = node_new(fb, AV_F_BETWEEN, pos, e);
    if (n != NULL) {
        n->field = *f;
        n->value = *lo;
        n->high = *hi;
    }
    return n;
}

av_filter *av_fb_startswith(av_fbuild *fb, int pos, const av_field *f, const av_value *prefix, av_err *e)
{
    if (f->kind != AV_FIELD_ATTR || prefix->type != AV_AT_STR) {
        av_filter_err(e, pos, "STARTSWITH needs a user attribute and a str literal");
        return NULL;
    }
    if (!leaf_count(fb, pos, e)) {
        return NULL;
    }
    av_filter *n = node_new(fb, AV_F_STARTSWITH, pos, e);
    if (n != NULL) {
        n->field = *f;
        n->value = *prefix;
    }
    return n;
}

av_filter *av_fb_exists(av_fbuild *fb, int pos, const av_field *f, av_err *e)
{
    if (!leaf_count(fb, pos, e)) {
        return NULL;
    }
    av_filter *n = node_new(fb, AV_F_EXISTS, pos, e);
    if (n != NULL) {
        n->field = *f;
    }
    return n;
}

av_filter *av_fb_typeof(av_fbuild *fb, int pos, const av_field *f, av_attr_type t, av_err *e)
{
    if (!av_type_filterable(t) && t != AV_AT_BYTES) {
        av_filter_err(e, pos, "unknown type in TYPEOF");
        return NULL;
    }
    if (!leaf_count(fb, pos, e)) {
        return NULL;
    }
    av_filter *n = node_new(fb, AV_F_TYPEOF, pos, e);
    if (n != NULL) {
        n->field = *f;
        n->type = t;
    }
    return n;
}

static bool depth_ok(int d, int pos, av_err *e)
{
    if (d > AV_FILTER_MAX_DEPTH) {
        av_filter_err(e, pos, "filter is nested deeper than %d levels", AV_FILTER_MAX_DEPTH);
        return false;
    }
    return true;
}

av_filter *av_fb_not(av_fbuild *fb, int pos, av_filter *kid, av_err *e)
{
    if (!depth_ok(kid->depth + 1, pos, e)) {
        return NULL;
    }
    av_filter *n = node_new(fb, AV_F_NOT, pos, e);
    if (n == NULL) {
        return NULL;
    }
    n->kids = av_arena_alloc(fb->arena, sizeof(av_filter *), 0);
    if (n->kids == NULL) {
        err_internal(e);
        return NULL;
    }
    n->kids[0] = kid;
    n->n_kids = 1;
    n->depth = kid->depth + 1;
    return n;
}

av_filter *av_fb_group(av_fbuild *fb, av_filter_kind kind, int pos, av_filter *const *kids, size_t n, av_err *e)
{
    if (n == 0 || (kind != AV_F_AND && kind != AV_F_OR)) {
        av_filter_err(e, pos, "empty or invalid AND/OR group");
        return NULL;
    }
    if (n == 1) {
        return kids[0];
    }
    int d = 0;
    for (size_t i = 0; i < n; i++) {
        if (kids[i]->depth > d) {
            d = kids[i]->depth;
        }
    }
    if (!depth_ok(d + 1, pos, e)) {
        return NULL;
    }
    av_filter *g = node_new(fb, kind, pos, e);
    if (g == NULL) {
        return NULL;
    }
    g->kids = av_arena_alloc(fb->arena, n * sizeof(av_filter *), 0);
    if (g->kids == NULL) {
        err_internal(e);
        return NULL;
    }
    memcpy(g->kids, kids, n * sizeof(av_filter *));
    g->n_kids = n;
    g->depth = d + 1;
    return g;
}

av_filter *av_filter_true(av_arena *a)
{
    av_filter *f = av_arena_calloc(a, sizeof(*f), 0);
    if (f != NULL) {
        f->kind = AV_F_TRUE;
        f->pos = -1;
        f->depth = 1;
    }
    return f;
}

/* ------------------------------------------------------------------------- */
/* equality                                                                  */
/* ------------------------------------------------------------------------- */

static bool field_equal(const av_field *a, const av_field *b)
{
    return a->kind == b->kind && strcmp(a->name, b->name) == 0;
}

bool av_filter_equal(const av_filter *a, const av_filter *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL || a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case AV_F_TRUE:
        return true;
    case AV_F_AND:
    case AV_F_OR:
    case AV_F_NOT:
        if (a->n_kids != b->n_kids) {
            return false;
        }
        for (size_t i = 0; i < a->n_kids; i++) {
            if (!av_filter_equal(a->kids[i], b->kids[i])) {
                return false;
            }
        }
        return true;
    case AV_F_CMP:
        return field_equal(&a->field, &b->field) && a->op == b->op && av_value_equal(&a->value, &b->value);
    case AV_F_BETWEEN:
        return field_equal(&a->field, &b->field) && av_value_equal(&a->value, &b->value) &&
               av_value_equal(&a->high, &b->high);
    case AV_F_STARTSWITH:
        return field_equal(&a->field, &b->field) && av_value_equal(&a->value, &b->value);
    case AV_F_EXISTS:
        return field_equal(&a->field, &b->field);
    case AV_F_TYPEOF:
        return field_equal(&a->field, &b->field) && a->type == b->type;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* printing                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} out_sink;

static void put(out_sink *s, char c)
{
    if (s->buf != NULL && s->len < s->cap) {
        s->buf[s->len] = c;
    }
    s->len++;
}

static void puts_(out_sink *s, const char *p)
{
    for (; *p != '\0'; p++) {
        put(s, *p);
    }
}

static void put_value(out_sink *s, const av_value *v)
{
    if (v->type == AV_AT_BOOL) {
        puts_(s, v->b[31] != 0 ? "true" : "false");
        return;
    }
    puts_(s, av_type_name(v->type));
    put(s, '(');
    if (v->type == AV_AT_STR) {
        put(s, '\'');
        for (size_t i = 0; i < v->len; i++) {
            if (v->b[i] == '\'') {
                put(s, '\'');
            }
            put(s, (char) v->b[i]);
        }
        put(s, '\'');
    } else {
        char tmp[AV_VALUE_FORMAT_MAX];
        (void) av_value_format(v, tmp, sizeof(tmp));
        puts_(s, tmp);
    }
    put(s, ')');
}

static const char *op_text(av_cmp_op op)
{
    static const char *const t[] = {"=", "!=", "<", "<=", ">", ">="};
    return t[op];
}

static void emit(out_sink *s, const av_filter *f)
{
    switch (f->kind) {
    case AV_F_TRUE:
        puts_(s, "true");
        break;
    case AV_F_AND:
    case AV_F_OR:
        put(s, '(');
        for (size_t i = 0; i < f->n_kids; i++) {
            if (i > 0) {
                puts_(s, f->kind == AV_F_AND ? " AND " : " OR ");
            }
            emit(s, f->kids[i]);
        }
        put(s, ')');
        break;
    case AV_F_NOT:
        puts_(s, "NOT ");
        emit(s, f->kids[0]);
        break;
    case AV_F_CMP:
        puts_(s, f->field.name);
        put(s, ' ');
        puts_(s, op_text(f->op));
        put(s, ' ');
        put_value(s, &f->value);
        break;
    case AV_F_BETWEEN:
        puts_(s, f->field.name);
        puts_(s, " BETWEEN ");
        put_value(s, &f->value);
        puts_(s, " AND ");
        put_value(s, &f->high);
        break;
    case AV_F_STARTSWITH:
        puts_(s, f->field.name);
        puts_(s, " STARTSWITH ");
        put_value(s, &f->value);
        break;
    case AV_F_EXISTS:
        puts_(s, "EXISTS(");
        puts_(s, f->field.name);
        put(s, ')');
        break;
    case AV_F_TYPEOF:
        puts_(s, "TYPEOF(");
        puts_(s, f->field.name);
        puts_(s, ") = ");
        puts_(s, av_type_name(f->type));
        break;
    }
}

char *av_filter_to_string(const av_filter *f, av_arena *a)
{
    out_sink measure = {NULL, 0, 0};
    emit(&measure, f);
    char *buf = av_arena_alloc(a, measure.len + 1, 1);
    if (buf == NULL) {
        return NULL;
    }
    out_sink w = {buf, measure.len, 0};
    emit(&w, f);
    buf[measure.len] = '\0';
    return buf;
}
