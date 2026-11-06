/*
 * filter/eval.c: evaluate a filter AST against attribute postings and docmap.
 */
#include "filter/eval.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    roaring_bitmap_t *bm;
    const uint8_t *prefix;
    size_t prefix_len;
} scan_ctx;

static bool scan_or_cb(const uint8_t *val, size_t len, const roaring_bitmap_t *postings, void *ctx)
{
    (void) val;
    (void) len;
    scan_ctx *sc = (scan_ctx *) ctx;
    roaring_bitmap_or_inplace(sc->bm, postings);
    return true;
}

static bool scan_startswith_cb(const uint8_t *val, size_t len, const roaring_bitmap_t *postings, void *ctx)
{
    scan_ctx *sc = (scan_ctx *) ctx;
    if (len < sc->prefix_len || memcmp(val, sc->prefix, sc->prefix_len) != 0) {
        return false; /* outside prefix range, stop scan */
    }
    roaring_bitmap_or_inplace(sc->bm, postings);
    return true;
}

typedef struct {
    const av_docmap *docmap;
    const uint8_t *target_key;
    const uint8_t *high_key;
    av_cmp_op op;
    bool is_between;
    roaring_bitmap_t *out;
} key_scan_ctx;

static bool key_cmp_cb(uint32_t id, void *ctx)
{
    key_scan_ctx *kc = (key_scan_ctx *) ctx;
    const uint8_t *k = av_docmap_key(kc->docmap, id);
    if (k == NULL)
        return true;

    if (kc->is_between) {
        if (memcmp(k, kc->target_key, 32) >= 0 && memcmp(k, kc->high_key, 32) <= 0) {
            roaring_bitmap_add(kc->out, id);
        }
        return true;
    }

    int c = memcmp(k, kc->target_key, 32);
    bool match = false;
    switch (kc->op) {
    case AV_OP_EQ:
        match = (c == 0);
        break;
    case AV_OP_NE:
        match = (c != 0);
        break;
    case AV_OP_LT:
        match = (c < 0);
        break;
    case AV_OP_LE:
        match = (c <= 0);
        break;
    case AV_OP_GT:
        match = (c > 0);
        break;
    case AV_OP_GE:
        match = (c >= 0);
        break;
    }
    if (match) {
        roaring_bitmap_add(kc->out, id);
    }
    return true;
}

static roaring_bitmap_t *eval_node(const av_filter *f, const av_attr_index *ix, const av_docmap *docmap,
                                   const roaring_bitmap_t *live, av_err *e)
{
    if (f == NULL || f->kind == AV_F_TRUE) {
        return live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();
    }

    switch (f->kind) {
    case AV_F_TRUE:
        return live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();

    case AV_F_AND: {
        if (f->n_kids == 0)
            return live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();
        roaring_bitmap_t *res = eval_node(f->kids[0], ix, docmap, live, e);
        if (res == NULL)
            return NULL;
        for (size_t i = 1; i < f->n_kids; i++) {
            if (roaring_bitmap_is_empty(res))
                break;
            roaring_bitmap_t *child = eval_node(f->kids[i], ix, docmap, live, e);
            if (child == NULL) {
                roaring_bitmap_free(res);
                return NULL;
            }
            roaring_bitmap_and_inplace(res, child);
            roaring_bitmap_free(child);
        }
        return res;
    }

    case AV_F_OR: {
        roaring_bitmap_t *res = roaring_bitmap_create();
        if (res == NULL) {
            av_err_set(e, "internal", "out of memory");
            return NULL;
        }
        for (size_t i = 0; i < f->n_kids; i++) {
            roaring_bitmap_t *child = eval_node(f->kids[i], ix, docmap, live, e);
            if (child == NULL) {
                roaring_bitmap_free(res);
                return NULL;
            }
            roaring_bitmap_or_inplace(res, child);
            roaring_bitmap_free(child);
        }
        return res;
    }

    case AV_F_NOT: {
        if (f->n_kids != 1) {
            av_err_set(e, "invalid_filter", "NOT requires exactly one child");
            return NULL;
        }
        roaring_bitmap_t *child = eval_node(f->kids[0], ix, docmap, live, e);
        if (child == NULL)
            return NULL;
        roaring_bitmap_t *res = live != NULL ? roaring_bitmap_andnot(live, child) : roaring_bitmap_create();
        roaring_bitmap_free(child);
        if (res == NULL) {
            av_err_set(e, "internal", "out of memory");
            return NULL;
        }
        return res;
    }

    case AV_F_EXISTS: {
        if (f->field.kind != AV_FIELD_ATTR) {
            /* System fields exist on all live entities */
            return live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();
        }
        const roaring_bitmap_t *p = av_attr_index_exists(ix, f->field.name);
        return p != NULL ? roaring_bitmap_copy(p) : roaring_bitmap_create();
    }

    case AV_F_TYPEOF: {
        if (f->field.kind == AV_FIELD_KEY) {
            return f->type == AV_AT_KEY && live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();
        }
        if (f->field.kind == AV_FIELD_OWNER || f->field.kind == AV_FIELD_CREATOR) {
            return f->type == AV_AT_ADDR && live != NULL ? roaring_bitmap_copy(live)
                                                         : roaring_bitmap_create();
        }
        if (f->field.kind == AV_FIELD_EXPIRES) {
            return f->type == AV_AT_U64 && live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();
        }
        const roaring_bitmap_t *p = av_attr_index_typed(ix, f->field.name, (uint8_t) f->type);
        return p != NULL ? roaring_bitmap_copy(p) : roaring_bitmap_create();
    }

    case AV_F_CMP: {
        if (f->field.kind == AV_FIELD_KEY) {
            if (f->op == AV_OP_EQ) {
                roaring_bitmap_t *res = roaring_bitmap_create();
                if (res == NULL) {
                    av_err_set(e, "internal", "out of memory");
                    return NULL;
                }
                av_docid id;
                if (av_docmap_lookup(docmap, f->value.b, &id)) {
                    if (live == NULL || roaring_bitmap_contains(live, id)) {
                        roaring_bitmap_add(res, id);
                    }
                }
                return res;
            }
            if (f->op == AV_OP_NE) {
                roaring_bitmap_t *res = live != NULL ? roaring_bitmap_copy(live) : roaring_bitmap_create();
                if (res == NULL) {
                    av_err_set(e, "internal", "out of memory");
                    return NULL;
                }
                av_docid id;
                if (av_docmap_lookup(docmap, f->value.b, &id)) {
                    roaring_bitmap_remove(res, id);
                }
                return res;
            }
            /* Inequality on key: scan live */
            roaring_bitmap_t *res = roaring_bitmap_create();
            if (res == NULL) {
                av_err_set(e, "internal", "out of memory");
                return NULL;
            }
            if (live != NULL) {
                key_scan_ctx kc = {docmap, f->value.b, NULL, f->op, false, res};
                roaring_iterate(live, key_cmp_cb, &kc);
            }
            return res;
        }

        const char *attr_name = f->field.name;
        uint8_t attr_type = (uint8_t) f->value.type;

        if (f->op == AV_OP_EQ) {
            const roaring_bitmap_t *p = av_attr_index_eq(ix, attr_name, attr_type, f->value.b, f->value.len);
            return p != NULL ? roaring_bitmap_copy(p) : roaring_bitmap_create();
        }

        if (f->op == AV_OP_NE) {
            const roaring_bitmap_t *typed = av_attr_index_typed(ix, attr_name, attr_type);
            if (typed == NULL)
                return roaring_bitmap_create();
            const roaring_bitmap_t *eq = av_attr_index_eq(ix, attr_name, attr_type, f->value.b, f->value.len);
            if (eq == NULL)
                return roaring_bitmap_copy(typed);
            return roaring_bitmap_andnot(typed, eq);
        }

        roaring_bitmap_t *res = roaring_bitmap_create();
        if (res == NULL) {
            av_err_set(e, "internal", "out of memory");
            return NULL;
        }
        scan_ctx sc = {res, NULL, 0};
        const uint8_t *lo = NULL;
        size_t lo_len = 0;
        bool lo_incl = false;
        const uint8_t *hi = NULL;
        size_t hi_len = 0;
        bool hi_incl = false;

        switch (f->op) {
        case AV_OP_LT:
            hi = f->value.b;
            hi_len = f->value.len;
            hi_incl = false;
            break;
        case AV_OP_LE:
            hi = f->value.b;
            hi_len = f->value.len;
            hi_incl = true;
            break;
        case AV_OP_GT:
            lo = f->value.b;
            lo_len = f->value.len;
            lo_incl = false;
            break;
        case AV_OP_GE:
            lo = f->value.b;
            lo_len = f->value.len;
            lo_incl = true;
            break;
        default:
            break;
        }

        av_attr_index_scan(ix, attr_name, attr_type, lo, lo_len, lo_incl, hi, hi_len, hi_incl, scan_or_cb,
                           &sc);
        return res;
    }

    case AV_F_BETWEEN: {
        if (f->field.kind == AV_FIELD_KEY) {
            roaring_bitmap_t *res = roaring_bitmap_create();
            if (res == NULL) {
                av_err_set(e, "internal", "out of memory");
                return NULL;
            }
            if (live != NULL) {
                key_scan_ctx kc = {docmap, f->value.b, f->high.b, AV_OP_EQ, true, res};
                roaring_iterate(live, key_cmp_cb, &kc);
            }
            return res;
        }
        roaring_bitmap_t *res = roaring_bitmap_create();
        if (res == NULL) {
            av_err_set(e, "internal", "out of memory");
            return NULL;
        }
        scan_ctx sc = {res, NULL, 0};
        av_attr_index_scan(ix, f->field.name, (uint8_t) f->value.type, f->value.b, f->value.len, true,
                           f->high.b, f->high.len, true, scan_or_cb, &sc);
        return res;
    }

    case AV_F_STARTSWITH: {
        if (f->value.type != AV_AT_STR) {
            av_err_set(e, "invalid_filter", "STARTSWITH requires str value");
            return NULL;
        }
        if (f->value.len == 0) {
            const roaring_bitmap_t *p = av_attr_index_typed(ix, f->field.name, AV_AT_STR);
            return p != NULL ? roaring_bitmap_copy(p) : roaring_bitmap_create();
        }
        roaring_bitmap_t *res = roaring_bitmap_create();
        if (res == NULL) {
            av_err_set(e, "internal", "out of memory");
            return NULL;
        }
        scan_ctx sc = {res, f->value.b, f->value.len};
        av_attr_index_scan(ix, f->field.name, AV_AT_STR, f->value.b, f->value.len, true, NULL, 0, false,
                           scan_startswith_cb, &sc);
        return res;
    }
    }

    return roaring_bitmap_create();
}

roaring_bitmap_t *av_filter_eval(const av_filter *f, const av_attr_index *ix, const av_docmap *docmap,
                                 const roaring_bitmap_t *live, uint64_t as_of_block, av_err *e)
{
    roaring_bitmap_t *res = eval_node(f, ix, docmap, live, e);
    if (res == NULL)
        return NULL;

    /* Always intersect with live bitmap */
    if (live != NULL) {
        roaring_bitmap_and_inplace(res, live);
    } else {
        roaring_bitmap_clear(res);
        return res;
    }

    /* Prune expired entities: expires_at_block <= as_of_block */
    uint32_t card = (uint32_t) roaring_bitmap_get_cardinality(res);
    if (card > 0 && docmap != NULL) {
        uint32_t *docids = malloc(card * sizeof(uint32_t));
        if (docids != NULL) {
            roaring_bitmap_to_uint32_array(res, docids);
            for (uint32_t i = 0; i < card; i++) {
                uint64_t up = 0, exp = 0;
                if (av_docmap_meta(docmap, docids[i], &up, &exp)) {
                    if (exp <= as_of_block) {
                        roaring_bitmap_remove(res, docids[i]);
                    }
                }
            }
            free(docids);
        }
    }

    return res;
}
