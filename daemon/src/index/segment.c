/*
 * index/segment.c: flat segment. See segment.h.
 */
#include "index/segment.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "varatt.h"

#include "store/arena_file.h"

#define DEFAULT_MAX_DOCIDS ((size_t) 1 << 28)

struct av_segment {
    av_seg_kind kind;
    av_vtype type;
    int dim;
    av_arena_file *data;
    av_arena_file *table; /* uint64_t offsets indexed by docid, 0 = absent */
    _Atomic size_t table_len;
    _Atomic size_t count;
};

av_status av_segment_create_flat(const char *path, av_vtype type, int dim, size_t reserve_bytes, av_segment **out,
                                 av_err *e)
{
    av_segment *s = calloc(1, sizeof *s);
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->kind = AV_SEG_FLAT;
    s->type = type;
    s->dim = dim;
    if (av_arena_file_open(path, reserve_bytes ? reserve_bytes : AV_SEG_DEFAULT_RESERVE, AV_AF_TRUNCATE, &s->data, e) !=
            AV_OK ||
        av_arena_file_open(NULL, DEFAULT_MAX_DOCIDS * sizeof(uint64_t), 0, &s->table, e) != AV_OK) {
        av_segment_destroy(s);
        return AV_ERR;
    }
    /* Offset 0 means "absent": burn the first 8 bytes. */
    if (av_arena_file_alloc(s->data, 8, 8, e) == NULL) {
        av_segment_destroy(s);
        return AV_ERR;
    }
    *out = s;
    return AV_OK;
}

void av_segment_destroy(av_segment *s)
{
    if (s == NULL)
        return;
    av_arena_file_close(s->data);
    av_arena_file_close(s->table);
    free(s);
}

av_seg_kind av_segment_kind(const av_segment *s)
{
    return s->kind;
}

av_status av_segment_insert(av_segment *s, av_docid id, const void *pgvec, av_err *e)
{
    size_t need = ((size_t) id + 1) * sizeof(uint64_t);
    if (need > av_arena_file_reserved(s->table)) {
        av_err_set(e, "internal", "docid %u exceeds the segment table", id);
        return AV_ERR;
    }
    if (av_pgvec_dim(s->type, pgvec) != s->dim) {
        av_err_set(e, "dimension_mismatch", "vector dimension differs from the segment (docid %u)", id);
        return AV_ERR;
    }
    if (av_arena_file_ensure(s->table, need, e) != AV_OK)
        return AV_ERR;
    uint64_t *table = av_arena_file_base(s->table);
    if (table[id] != 0) {
        av_err_set(e, "internal", "docid %u is already in the segment", id);
        return AV_ERR;
    }
    size_t n = VARSIZE(pgvec);
    void *dst = av_arena_file_alloc(s->data, n, 8, e);
    if (dst == NULL)
        return AV_ERR;
    memcpy(dst, pgvec, n);
    table[id] = (uint64_t) ((uint8_t *) dst - (uint8_t *) av_arena_file_base(s->data));
    size_t len = atomic_load_explicit(&s->table_len, memory_order_relaxed);
    if ((size_t) id + 1 > len)
        atomic_store_explicit(&s->table_len, (size_t) id + 1, memory_order_release);
    atomic_fetch_add_explicit(&s->count, 1, memory_order_release);
    return AV_OK;
}

size_t av_segment_count(const av_segment *s)
{
    return atomic_load_explicit(&s->count, memory_order_acquire);
}

const void *av_segment_get(const av_segment *s, av_docid id)
{
    if ((size_t) id >= atomic_load_explicit(&s->table_len, memory_order_acquire))
        return NULL;
    uint64_t off = ((const uint64_t *) av_arena_file_base(s->table))[id];
    return off != 0 ? (const uint8_t *) av_arena_file_base(s->data) + off : NULL;
}

/* ---- bounded max-heap of the k best (smallest) hits; "worse" = larger (distance, id), NaN largest ---- */

static bool hit_worse(const av_hit *a, const av_hit *b)
{
    bool an = isnan(a->distance), bn = isnan(b->distance);
    if (an != bn)
        return an;
    if (!an && a->distance != b->distance)
        return a->distance > b->distance;
    return a->id > b->id;
}

static void sift_down(av_hit *h, size_t n, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, w = i;
        if (l < n && hit_worse(&h[l], &h[w]))
            w = l;
        if (r < n && hit_worse(&h[r], &h[w]))
            w = r;
        if (w == i)
            return;
        av_hit t = h[i];
        h[i] = h[w];
        h[w] = t;
        i = w;
    }
}

static void sift_up(av_hit *h, size_t i)
{
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!hit_worse(&h[i], &h[p]))
            return;
        av_hit t = h[i];
        h[i] = h[p];
        h[p] = t;
        i = p;
    }
}

typedef struct {
    const av_segment *s;
    const void *query;
    const av_opclass *oc;
    av_hit *heap;
    size_t k, n;
} scan_ctx;

static void scan_one(scan_ctx *c, av_docid id)
{
    const void *v = av_segment_get(c->s, id);
    if (v == NULL)
        return;
    av_hit h = {id, c->oc->distance(c->query, v)};
    if (c->n < c->k) {
        c->heap[c->n] = h;
        sift_up(c->heap, c->n++);
    } else if (hit_worse(&c->heap[0], &h)) {
        c->heap[0] = h;
        sift_down(c->heap, c->n, 0);
    }
}

static bool scan_cb(uint32_t id, void *p)
{
    scan_one(p, id);
    return true;
}

av_status av_segment_scan_exact(const av_segment *s, const void *query, const av_opclass *oc,
                                const roaring_bitmap_t *cand, size_t k, av_hit *out, size_t *n_out, av_err *e)
{
    *n_out = 0;
    if (oc->type != s->type) {
        av_err_set(e, "invalid_argument", "opclass %s does not match the segment column type", oc->name);
        return AV_ERR;
    }
    if (av_pgvec_dim(s->type, query) != s->dim) {
        av_err_set(e, "dimension_mismatch", "query dimension %d differs from column dimension %d",
                   av_pgvec_dim(s->type, query), s->dim);
        return AV_ERR;
    }
    if (k == 0)
        return AV_OK;
    scan_ctx c = {s, query, oc, out, k, 0};
    if (cand != NULL) {
        roaring_iterate(cand, scan_cb, &c);
    } else {
        size_t len = atomic_load_explicit(&s->table_len, memory_order_acquire);
        for (size_t i = 0; i < len; i++)
            scan_one(&c, (av_docid) i);
    }
    /* Heap-sort in place: repeatedly move the worst to the end -> ascending order. */
    size_t n = c.n;
    for (size_t m = n; m > 1; m--) {
        av_hit t = out[0];
        out[0] = out[m - 1];
        out[m - 1] = t;
        sift_down(out, m - 1, 0);
    }
    *n_out = n;
    return AV_OK;
}
