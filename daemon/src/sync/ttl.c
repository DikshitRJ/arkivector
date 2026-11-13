/*
 * sync/ttl.c: lazy min-heap TTL scheduler. See ttl.h.
 */
#include "sync/ttl.h"

#include <roaring.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t expires_at;
    uint32_t docid;
    uint32_t gen;
} entry;

struct av_ttl {
    entry *heap;
    size_t n, cap;
    uint32_t *gen;    /* per docid */
    uint64_t *expiry; /* per docid; 0 = unscheduled */
    size_t ndoc;      /* capacity of gen/expiry */
};

av_ttl *av_ttl_create(void)
{
    return calloc(1, sizeof(av_ttl));
}

void av_ttl_destroy(av_ttl *t)
{
    if (t == NULL) {
        return;
    }
    free(t->heap);
    free(t->gen);
    free(t->expiry);
    free(t);
}

static int ensure_doc(av_ttl *t, uint32_t docid)
{
    if ((size_t) docid < t->ndoc) {
        return 0;
    }
    size_t want = t->ndoc ? t->ndoc : 1024;
    while (want <= (size_t) docid) {
        want *= 2;
    }
    uint32_t *g = realloc(t->gen, want * sizeof(uint32_t));
    if (g == NULL) {
        return -1;
    }
    t->gen = g;
    uint64_t *x = realloc(t->expiry, want * sizeof(uint64_t));
    if (x == NULL) {
        return -1;
    }
    t->expiry = x;
    memset(t->gen + t->ndoc, 0, (want - t->ndoc) * sizeof(uint32_t));
    memset(t->expiry + t->ndoc, 0, (want - t->ndoc) * sizeof(uint64_t));
    t->ndoc = want;
    return 0;
}

static int less(const entry *a, const entry *b)
{
    if (a->expires_at != b->expires_at) {
        return a->expires_at < b->expires_at;
    }
    return a->docid < b->docid;
}

static void sift_up(av_ttl *t, size_t i)
{
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!less(&t->heap[i], &t->heap[p])) {
            break;
        }
        entry tmp = t->heap[i];
        t->heap[i] = t->heap[p];
        t->heap[p] = tmp;
        i = p;
    }
}

static void sift_down(av_ttl *t, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && less(&t->heap[l], &t->heap[m])) {
            m = l;
        }
        if (r < t->n && less(&t->heap[r], &t->heap[m])) {
            m = r;
        }
        if (m == i) {
            return;
        }
        entry tmp = t->heap[i];
        t->heap[i] = t->heap[m];
        t->heap[m] = tmp;
        i = m;
    }
}

static int push(av_ttl *t, entry e)
{
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 1024;
        entry *h = realloc(t->heap, nc * sizeof(entry));
        if (h == NULL) {
            return -1;
        }
        t->heap = h;
        t->cap = nc;
    }
    t->heap[t->n++] = e;
    sift_up(t, t->n - 1);
    return 0;
}

av_status av_ttl_set(av_ttl *t, uint32_t docid, uint64_t expires_at)
{
    if (ensure_doc(t, docid) != 0) {
        return AV_ERR;
    }
    t->gen[docid]++;
    t->expiry[docid] = expires_at;
    if (expires_at == 0) {
        return AV_OK;
    }
    entry e = {expires_at, docid, t->gen[docid]};
    return push(t, e) == 0 ? AV_OK : AV_ERR;
}

void av_ttl_remove(av_ttl *t, uint32_t docid)
{
    if ((size_t) docid < t->ndoc) {
        t->gen[docid]++;
        t->expiry[docid] = 0;
    }
}

static int is_current(const av_ttl *t, const entry *e)
{
    return (size_t) e->docid < t->ndoc && t->gen[e->docid] == e->gen && t->expiry[e->docid] == e->expires_at;
}

size_t av_ttl_pop_expired(av_ttl *t, uint64_t block, uint32_t *out, size_t max)
{
    size_t k = 0;
    while (k < max && t->n > 0 && t->heap[0].expires_at <= block) {
        entry e = t->heap[0];
        t->heap[0] = t->heap[--t->n];
        if (t->n > 0) {
            sift_down(t, 0);
        }
        if (!is_current(t, &e)) {
            continue; /* stale: extended, re-set or removed since */
        }
        t->expiry[e.docid] = 0;
        t->gen[e.docid]++;
        out[k++] = e.docid;
    }
    return k;
}

uint64_t av_ttl_expiry(const av_ttl *t, uint32_t docid)
{
    return (size_t) docid < t->ndoc ? t->expiry[docid] : 0;
}

size_t av_ttl_heap_size(const av_ttl *t)
{
    return t->n;
}

av_status av_ttl_compact(av_ttl *t)
{
    size_t w = 0;
    for (size_t i = 0; i < t->n; i++) {
        if (is_current(t, &t->heap[i])) {
            t->heap[w++] = t->heap[i];
        }
    }
    t->n = w;
    for (size_t i = t->n / 2 + 1; i-- > 0;) {
        sift_down(t, i);
    }
    return AV_OK;
}

size_t av_ttl_count_due(const av_ttl *t, uint64_t block, const struct roaring_bitmap_s *in)
{
    if (t == NULL || t->n == 0 || t->heap[0].expires_at > block)
        return 0;
    /* iterative DFS over the heap, descending only into due nodes */
    size_t count = 0;
    size_t cap = 64, top = 0;
    size_t *stack = malloc(cap * sizeof *stack);
    if (stack == NULL)
        return 0;
    stack[top++] = 0;
    while (top > 0) {
        size_t i = stack[--top];
        const entry *e = &t->heap[i];
        if (e->expires_at > block)
            continue;
        if (is_current(t, e) &&
            (in == NULL || roaring_bitmap_contains((const roaring_bitmap_t *) in, e->docid)))
            count++;
        for (size_t c = 2 * i + 1; c <= 2 * i + 2 && c < t->n; c++) {
            if (top == cap) {
                size_t *ns = realloc(stack, cap * 2 * sizeof *ns);
                if (ns == NULL) {
                    free(stack);
                    return count;
                }
                stack = ns;
                cap *= 2;
            }
            stack[top++] = c;
        }
    }
    free(stack);
    return count;
}
