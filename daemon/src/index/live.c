/*
 * index/live.c: see live.h.
 */
#include "index/live.h"

#include <stdatomic.h>
#include <stdlib.h>

struct av_live_snap {
    _Atomic int refs;
    roaring_bitmap_t *bm;
};

struct av_live {
    av_live_snap *cur;
    roaring_bitmap_t *working;
};

static av_live_snap *snap_new(roaring_bitmap_t *bm)
{
    av_live_snap *s = malloc(sizeof *s);
    if (s == NULL)
        return NULL;
    atomic_init(&s->refs, 1);
    s->bm = bm;
    return s;
}

av_live *av_live_create(void)
{
    av_live *l = calloc(1, sizeof *l);
    if (l == NULL)
        return NULL;
    roaring_bitmap_t *bm = roaring_bitmap_create();
    l->cur = bm != NULL ? snap_new(bm) : NULL;
    if (l->cur == NULL) {
        roaring_bitmap_free(bm);
        free(l);
        return NULL;
    }
    return l;
}

void av_live_snap_release(av_live_snap *s)
{
    if (s == NULL)
        return;
    if (atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) == 1) {
        roaring_bitmap_free(s->bm);
        free(s);
    }
}

void av_live_destroy(av_live *l)
{
    if (l == NULL)
        return;
    roaring_bitmap_free(l->working);
    av_live_snap_release(l->cur);
    free(l);
}

const roaring_bitmap_t *av_live_current(const av_live *l)
{
    return l->cur->bm;
}

av_live_snap *av_live_acquire(av_live *l)
{
    atomic_fetch_add_explicit(&l->cur->refs, 1, memory_order_relaxed);
    return l->cur;
}

const roaring_bitmap_t *av_live_snap_bitmap(const av_live_snap *s)
{
    return s->bm;
}

roaring_bitmap_t *av_live_working(av_live *l)
{
    if (l->working == NULL)
        l->working = roaring_bitmap_copy(l->cur->bm);
    return l->working;
}

void av_live_publish(av_live *l)
{
    if (l->working == NULL)
        return;
    av_live_snap *n = snap_new(l->working);
    if (n == NULL)
        abort(); /* cannot publish half a block: fail loudly rather than serve a stale bitmap */
    l->working = NULL;
    av_live_snap *old = l->cur;
    l->cur = n;
    av_live_snap_release(old);
}

void av_live_discard(av_live *l)
{
    roaring_bitmap_free(l->working);
    l->working = NULL;
}
