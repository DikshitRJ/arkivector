/*
 * index/flat.c: exact flat scan implementation.
 */
#include "index/flat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "codec/vtype.h"

typedef struct {
    const av_segment *s;
    const av_docmap *docmap;
    const void *query;
    const av_opclass *oc;
    av_hit *heap;
    size_t limit; /* offset + k */
    size_t n;
} flat_scan_ctx;

static bool hit_worse(const av_hit *a, const av_hit *b, const av_docmap *dm)
{
    bool an = isnan(a->distance), bn = isnan(b->distance);
    if (an != bn)
        return an;
    if (!an && a->distance != b->distance)
        return a->distance > b->distance;
    if (dm != NULL) {
        const uint8_t *ka = av_docmap_key(dm, a->id);
        const uint8_t *kb = av_docmap_key(dm, b->id);
        if (ka != NULL && kb != NULL) {
            int c = memcmp(ka, kb, 32);
            if (c != 0)
                return c > 0;
        }
    }
    return a->id > b->id;
}

static void sift_down(av_hit *h, size_t n, size_t i, const av_docmap *dm)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, w = i;
        if (l < n && hit_worse(&h[l], &h[w], dm))
            w = l;
        if (r < n && hit_worse(&h[r], &h[w], dm))
            w = r;
        if (w == i)
            return;
        av_hit t = h[i];
        h[i] = h[w];
        h[w] = t;
        i = w;
    }
}

static void sift_up(av_hit *h, size_t i, const av_docmap *dm)
{
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!hit_worse(&h[i], &h[p], dm))
            return;
        av_hit t = h[i];
        h[i] = h[p];
        h[p] = t;
        i = p;
    }
}

static void scan_one(flat_scan_ctx *c, av_docid id)
{
    const void *v = av_segment_get(c->s, id);
    if (v == NULL)
        return;
    av_hit h = {id, c->oc->distance(c->query, v)};
    if (c->n < c->limit) {
        c->heap[c->n] = h;
        sift_up(c->heap, c->n++, c->docmap);
    } else if (hit_worse(&c->heap[0], &h, c->docmap)) {
        c->heap[0] = h;
        sift_down(c->heap, c->n, 0, c->docmap);
    }
}

static bool scan_cb(uint32_t id, void *p)
{
    scan_one((flat_scan_ctx *) p, (av_docid) id);
    return true;
}

av_status av_flat_scan(const av_segment *s, const av_docmap *docmap, const void *query,
                       const av_opclass *oc, const roaring_bitmap_t *cand,
                       size_t offset, size_t k, av_hit *out, size_t *n_out, av_err *e)
{
    *n_out = 0;
    if (k == 0)
        return AV_OK;

    size_t limit = offset + k;
    av_hit *heap = malloc(limit * sizeof(av_hit));
    if (heap == NULL) {
        av_err_set(e, "internal", "out of memory for heap");
        return AV_ERR;
    }

    flat_scan_ctx c = {s, docmap, query, oc, heap, limit, 0};
    if (cand != NULL) {
        roaring_iterate(cand, scan_cb, &c);
    } else {
        size_t count = av_segment_count(s);
        for (size_t i = 0; i < count; i++) {
            scan_one(&c, (av_docid) i);
        }
    }

    /* Heap sort into ascending order */
    size_t n = c.n;
    for (size_t m = n; m > 1; m--) {
        av_hit t = heap[0];
        heap[0] = heap[m - 1];
        heap[m - 1] = t;
        sift_down(heap, m - 1, 0, docmap);
    }

    if (n <= offset) {
        *n_out = 0;
    } else {
        size_t available = n - offset;
        size_t take = available < k ? available : k;
        memcpy(out, &heap[offset], take * sizeof(av_hit));
        *n_out = take;
    }

    free(heap);
    return AV_OK;
}
