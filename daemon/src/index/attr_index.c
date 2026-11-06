/*
 * index/attr_index.c: see attr_index.h.
 */
#define _GNU_SOURCE
#include "index/attr_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"

#define FTAB_SIZE 256u

typedef struct {
    uint8_t *val;
    size_t len;
    roaring_bitmap_t *bm; /* NULL: emptied, dropped at the next flush */
} entry;

typedef struct {
    uint8_t *val;
    size_t len;
    av_docid id;
} fresh_ent;

typedef struct field {
    char name[AV_ATTR_NAME_MAX + 1];
    uint8_t type;
    roaring_bitmap_t *typed;
    entry *ents;
    size_t n, cap;
    size_t n_dead;
    fresh_ent *fresh;
    size_t n_fresh, cap_fresh;
    struct field *next;
} field;

typedef struct nameent {
    char name[AV_ATTR_NAME_MAX + 1];
    roaring_bitmap_t *exists;
    struct nameent *next;
} nameent;

struct av_attr_index {
    field *ftab[FTAB_SIZE];
    nameent *ntab[FTAB_SIZE];
};

int av_attr_value_cmp(uint8_t type, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    if ((type == AV_AT_I32 || type == AV_AT_DEC) && alen == 32 && blen == 32) {
        bool an = (a[0] & 0x80) != 0, bn = (b[0] & 0x80) != 0;
        if (an != bn)
            return an ? -1 : 1;
    }
    size_t m = alen < blen ? alen : blen;
    int c = m ? memcmp(a, b, m) : 0;
    if (c != 0)
        return c;
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static unsigned hash_name(const char *name, unsigned seed)
{
    unsigned h = 2166136261u ^ seed;
    for (const char *p = name; *p; p++)
        h = (h ^ (unsigned char) *p) * 16777619u;
    return h;
}

av_attr_index *av_attr_index_create(void)
{
    return calloc(1, sizeof(av_attr_index));
}

static void field_free(field *f)
{
    for (size_t i = 0; i < f->n; i++) {
        free(f->ents[i].val);
        roaring_bitmap_free(f->ents[i].bm);
    }
    for (size_t i = 0; i < f->n_fresh; i++)
        free(f->fresh[i].val);
    free(f->ents);
    free(f->fresh);
    roaring_bitmap_free(f->typed);
    free(f);
}

void av_attr_index_destroy(av_attr_index *ix)
{
    if (ix == NULL)
        return;
    for (unsigned i = 0; i < FTAB_SIZE; i++) {
        for (field *f = ix->ftab[i], *nx; f != NULL; f = nx) {
            nx = f->next;
            field_free(f);
        }
        for (nameent *n = ix->ntab[i], *nx; n != NULL; n = nx) {
            nx = n->next;
            roaring_bitmap_free(n->exists);
            free(n);
        }
    }
    free(ix);
}

static field *find_field(const av_attr_index *ix, const char *name, uint8_t type)
{
    for (field *f = ix->ftab[hash_name(name, type) % FTAB_SIZE]; f != NULL; f = f->next)
        if (f->type == type && strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

static field *get_field(av_attr_index *ix, const char *name, uint8_t type)
{
    field *f = find_field(ix, name, type);
    if (f != NULL)
        return f;
    f = calloc(1, sizeof *f);
    if (f == NULL)
        return NULL;
    f->typed = roaring_bitmap_create();
    if (f->typed == NULL) {
        free(f);
        return NULL;
    }
    (void) snprintf(f->name, sizeof f->name, "%s", name);
    f->type = type;
    unsigned h = hash_name(name, type) % FTAB_SIZE;
    f->next = ix->ftab[h];
    ix->ftab[h] = f;
    return f;
}

static nameent *find_name(const av_attr_index *ix, const char *name)
{
    for (nameent *n = ix->ntab[hash_name(name, 0) % FTAB_SIZE]; n != NULL; n = n->next)
        if (strcmp(n->name, name) == 0)
            return n;
    return NULL;
}

static nameent *get_name(av_attr_index *ix, const char *name)
{
    nameent *n = find_name(ix, name);
    if (n != NULL)
        return n;
    n = calloc(1, sizeof *n);
    if (n == NULL)
        return NULL;
    n->exists = roaring_bitmap_create();
    if (n->exists == NULL) {
        free(n);
        return NULL;
    }
    (void) snprintf(n->name, sizeof n->name, "%s", name);
    unsigned h = hash_name(name, 0) % FTAB_SIZE;
    n->next = ix->ntab[h];
    ix->ntab[h] = n;
    return n;
}

static bool value_indexed(uint8_t type, size_t len)
{
    return !(type == AV_AT_BYTES && len > AV_ATTR_STR_MAX);
}

/* Index of the first entry >= val, and whether it is equal. */
static size_t lower_bound(const field *f, const uint8_t *val, size_t len, bool *eq)
{
    size_t lo = 0, hi = f->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (av_attr_value_cmp(f->type, f->ents[mid].val, f->ents[mid].len, val, len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *eq = lo < f->n && av_attr_value_cmp(f->type, f->ents[lo].val, f->ents[lo].len, val, len) == 0;
    return lo;
}

static uint8_t *dup_bytes(const uint8_t *v, size_t len)
{
    uint8_t *p = malloc(len ? len : 1);
    if (p != NULL && len > 0)
        memcpy(p, v, len);
    return p;
}

static av_status oom(av_err *e)
{
    av_err_set(e, "internal", "out of memory in attribute index");
    return AV_ERR;
}

av_status av_attr_index_insert(av_attr_index *ix, av_docid id, const char *name, uint8_t type, const uint8_t *val,
                               size_t len, av_err *e)
{
    field *f = get_field(ix, name, type);
    nameent *n = get_name(ix, name);
    if (f == NULL || n == NULL)
        return oom(e);
    roaring_bitmap_add(f->typed, id);
    roaring_bitmap_add(n->exists, id);
    if (!value_indexed(type, len))
        return AV_OK;
    bool eq;
    size_t pos = lower_bound(f, val, len, &eq);
    if (eq) {
        entry *en = &f->ents[pos];
        if (en->bm == NULL) {
            en->bm = roaring_bitmap_create();
            if (en->bm == NULL)
                return oom(e);
            f->n_dead--;
        }
        roaring_bitmap_add(en->bm, id);
        return AV_OK;
    }
    if (f->n_fresh == f->cap_fresh) {
        size_t nc = f->cap_fresh ? f->cap_fresh * 2 : 16;
        fresh_ent *nf = realloc(f->fresh, nc * sizeof *nf);
        if (nf == NULL)
            return oom(e);
        f->fresh = nf;
        f->cap_fresh = nc;
    }
    uint8_t *copy = dup_bytes(val, len);
    if (copy == NULL)
        return oom(e);
    f->fresh[f->n_fresh++] = (fresh_ent) {copy, len, id};
    return AV_OK;
}

static int fresh_cmp(const void *a, const void *b, void *arg)
{
    const fresh_ent *x = a, *y = b;
    int c = av_attr_value_cmp(*(const uint8_t *) arg, x->val, x->len, y->val, y->len);
    if (c != 0)
        return c;
    return x->id < y->id ? -1 : (x->id > y->id ? 1 : 0);
}

static av_status flush_field(field *f, av_err *e)
{
    if (f->n_fresh == 0 && f->n_dead == 0)
        return AV_OK;
    uint8_t type = f->type;
    if (f->n_fresh > 1)
        qsort_r(f->fresh, f->n_fresh, sizeof *f->fresh, fresh_cmp, &type);
    /* Count distinct fresh values to size the merged array. */
    size_t distinct = 0;
    for (size_t i = 0; i < f->n_fresh; i++)
        if (i == 0 || av_attr_value_cmp(type, f->fresh[i - 1].val, f->fresh[i - 1].len, f->fresh[i].val,
                                        f->fresh[i].len) != 0)
            distinct++;
    size_t live = f->n - f->n_dead;
    entry *out = malloc((live + distinct ? live + distinct : 1) * sizeof *out);
    if (out == NULL)
        return oom(e);
    size_t no = 0, i = 0, j = 0;
    while (i < f->n || j < f->n_fresh) {
        while (i < f->n && f->ents[i].bm == NULL) {
            free(f->ents[i].val);
            i++;
        }
        if (i >= f->n && j >= f->n_fresh)
            break;
        bool take_main = j >= f->n_fresh || (i < f->n && av_attr_value_cmp(type, f->ents[i].val, f->ents[i].len,
                                                                          f->fresh[j].val, f->fresh[j].len) < 0);
        if (take_main) {
            out[no++] = f->ents[i++];
            continue;
        }
        /* Group the run of equal fresh values into one posting. */
        entry en = {f->fresh[j].val, f->fresh[j].len, roaring_bitmap_create()};
        if (en.bm == NULL) {
            free(out);
            return oom(e);
        }
        size_t k = j;
        while (k < f->n_fresh &&
               av_attr_value_cmp(type, f->fresh[k].val, f->fresh[k].len, en.val, en.len) == 0) {
            roaring_bitmap_add(en.bm, f->fresh[k].id);
            if (k != j)
                free(f->fresh[k].val);
            k++;
        }
        j = k;
        out[no++] = en;
    }
    free(f->ents);
    f->ents = out;
    f->n = no;
    f->cap = live + distinct;
    f->n_dead = 0;
    f->n_fresh = 0;
    return AV_OK;
}

av_status av_attr_index_flush(av_attr_index *ix, av_err *e)
{
    for (unsigned b = 0; b < FTAB_SIZE; b++)
        for (field *f = ix->ftab[b]; f != NULL; f = f->next)
            if (flush_field(f, e) != AV_OK)
                return AV_ERR;
    return AV_OK;
}

av_status av_attr_index_remove(av_attr_index *ix, av_docid id, const char *name, uint8_t type, const uint8_t *val,
                               size_t len, av_err *e)
{
    field *f = find_field(ix, name, type);
    if (f == NULL)
        return AV_OK;
    roaring_bitmap_remove(f->typed, id);
    nameent *n = find_name(ix, name);
    if (n != NULL)
        roaring_bitmap_remove(n->exists, id);
    if (!value_indexed(type, len))
        return AV_OK;
    bool eq;
    size_t pos = lower_bound(f, val, len, &eq);
    if (!eq && f->n_fresh > 0) {
        if (flush_field(f, e) != AV_OK)
            return AV_ERR;
        pos = lower_bound(f, val, len, &eq);
    }
    if (!eq || f->ents[pos].bm == NULL)
        return AV_OK;
    entry *en = &f->ents[pos];
    roaring_bitmap_remove(en->bm, id);
    if (roaring_bitmap_is_empty(en->bm)) {
        roaring_bitmap_free(en->bm);
        en->bm = NULL;
        f->n_dead++;
    }
    return AV_OK;
}

const roaring_bitmap_t *av_attr_index_exists(const av_attr_index *ix, const char *name)
{
    const nameent *n = find_name(ix, name);
    return n != NULL ? n->exists : NULL;
}

const roaring_bitmap_t *av_attr_index_typed(const av_attr_index *ix, const char *name, uint8_t type)
{
    const field *f = find_field(ix, name, type);
    return f != NULL ? f->typed : NULL;
}

const roaring_bitmap_t *av_attr_index_eq(const av_attr_index *ix, const char *name, uint8_t type, const uint8_t *val,
                                         size_t len)
{
    const field *f = find_field(ix, name, type);
    if (f == NULL)
        return NULL;
    bool eq;
    size_t pos = lower_bound(f, val, len, &eq);
    return eq ? f->ents[pos].bm : NULL;
}

void av_attr_index_scan(const av_attr_index *ix, const char *name, uint8_t type, const uint8_t *lo, size_t lo_len,
                        bool lo_incl, const uint8_t *hi, size_t hi_len, bool hi_incl, av_attr_scan_cb cb,
                        void *ctx)
{
    const field *f = find_field(ix, name, type);
    if (f == NULL)
        return;
    size_t pos = 0;
    if (lo != NULL) {
        bool eq;
        pos = lower_bound(f, lo, lo_len, &eq);
        if (eq && !lo_incl)
            pos++;
    }
    for (; pos < f->n; pos++) {
        const entry *en = &f->ents[pos];
        if (hi != NULL) {
            int c = av_attr_value_cmp(type, en->val, en->len, hi, hi_len);
            if (c > 0 || (c == 0 && !hi_incl))
                break;
        }
        if (en->bm == NULL)
            continue;
        if (!cb(en->val, en->len, en->bm, ctx))
            break;
    }
}

size_t av_attr_index_value_count(const av_attr_index *ix, const char *name, uint8_t type)
{
    const field *f = find_field(ix, name, type);
    return f != NULL ? f->n - f->n_dead : 0;
}
