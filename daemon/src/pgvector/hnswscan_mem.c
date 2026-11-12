/*
 * pgvector/hnswscan_mem.c: In-memory HNSW scan over arena graphs using pgvector's
 * GetScanItems and ResumeScanItems.
 */
#include "postgres.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"

#include "pgvector/hnswscan_mem.h"

static int hit_cmp(const void *a, const void *b)
{
    const av_hit *x = (const av_hit *) a;
    const av_hit *y = (const av_hit *) b;
    bool xnan = isnan(x->distance);
    bool ynan = isnan(y->distance);
    if (xnan != ynan)
        return xnan ? 1 : -1;
    if (x->distance < y->distance)
        return -1;
    if (x->distance > y->distance)
        return 1;
    return (x->id > y->id) - (x->id < y->id);
}

static List *
GetScanItemsMem(AvHnswScanState *so)
{
    char *base = so->base;
    HnswGraph *graph = so->graph;
    HnswElement entryPoint;
    List *ep;
    List *w;

    entryPoint = HnswPtrAccess(base, graph->entryPoint);
    if (entryPoint == NULL)
        return NIL;

    ep = list_make1(HnswEntryCandidate(base, entryPoint, &so->q, NULL, &so->support, false));

    for (int lc = entryPoint->level; lc >= 1; lc--)
        ep = HnswSearchLayer(base, &so->q, ep, 1, lc, NULL, &so->support, so->m, false, NULL, NULL, NULL, true, NULL, &so->searchState);

    w = HnswSearchLayer(base, &so->q, ep, so->ef_search, 0, NULL, &so->support, so->m, false, NULL,
                        &so->v,
                        so->iterative_scan != HNSW_ITERATIVE_SCAN_OFF ? &so->discarded : NULL,
                        true, &so->tuples, &so->searchState);
    return w;
}

static List *
ResumeScanItemsMem(AvHnswScanState *so)
{
    List *ep = NIL;

    if (so->discarded == NULL)
        return NIL;

    while (!pairingheap_is_empty(so->discarded) && list_length(ep) < so->ef_search)
    {
        HnswSearchCandidate *sc = HnswGetSearchCandidate(w_node, pairingheap_remove_first(so->discarded));
        ep = lappend(ep, sc);
    }

    if (list_length(ep) == 0)
        return NIL;

    return HnswSearchLayer(so->base, &so->q, ep, so->ef_search, 0, NULL, &so->support, so->m, false, NULL,
                          &so->v, &so->discarded, false, &so->tuples, &so->searchState);
}

typedef struct {
    av_hit *items;
    size_t count;
    size_t cap;
} hit_acc;

static bool acc_add(hit_acc *acc, av_docid id, double distance)
{
    if (acc->count >= acc->cap) {
        size_t ncap = acc->cap == 0 ? 64 : acc->cap * 2;
        av_hit *nitems = realloc(acc->items, ncap * sizeof(av_hit));
        if (nitems == NULL)
            return false;
        acc->items = nitems;
        acc->cap = ncap;
    }
    acc->items[acc->count].id = id;
    acc->items[acc->count].distance = distance;
    acc->count++;
    return true;
}

av_status av_hnsw_scan_mem(char *base, HnswGraph *graph, HnswSupport *support, int m,
                           Datum query_val, int ef_search, int iterative_mode,
                           int max_scan_tuples, const roaring_bitmap_t *cand,
                           size_t offset, size_t k, av_hit *out, size_t *n_out,
                           av_err *e)
{
    *n_out = 0;
    if (k == 0)
        return AV_OK;

    MemoryContext oldCtx = CurrentMemoryContext;
    MemoryContext tmpCtx = AllocSetContextCreate(oldCtx, "hnsw_scan_mem", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(tmpCtx);

    AvHnswScanState so;
    memset(&so, 0, sizeof(so));
    so.base = base;
    so.graph = graph;
    so.support = *support;
    so.q.value = query_val;
    so.m = m > 0 ? m : HNSW_DEFAULT_M;
    so.ef_search = ef_search > 0 ? ef_search : HNSW_DEFAULT_EF_SEARCH;
    so.iterative_scan = iterative_mode;
    so.max_scan_tuples = max_scan_tuples > 0 ? max_scan_tuples : hnsw_max_scan_tuples;
    so.max_memory = (Size) ((double) work_mem * hnsw_scan_mem_multiplier * 1024.0 + 256.0);
    so.tuples = 0;
    so.previous_distance = -INFINITY; /* upstream hnswscan.c: negative distances (<#>) must pass */
    so.tmpCtx = tmpCtx;

    HnswInitSearchState(&so.searchState, NULL, false);

    bool readonly = av_lwlock_readonly_arena;
    if (!readonly) {
        LWLockAcquire(&graph->entryWaitLock, LW_EXCLUSIVE);
        LWLockRelease(&graph->entryWaitLock);
        LWLockAcquire(&graph->entryLock, LW_SHARED);
    }

    so.w = GetScanItemsMem(&so);

    if (!readonly) {
        LWLockRelease(&graph->entryLock);
    }

    hit_acc acc;
    memset(&acc, 0, sizeof(acc));

    size_t target_count = offset + k;
    bool done_extra_batch = false;

    /* so.discarded stays NULL with iterative_scan = off and for an empty graph: never dereference it then */
    while (so.w != NIL || (so.iterative_scan != HNSW_ITERATIVE_SCAN_OFF && so.discarded != NULL &&
                           !pairingheap_is_empty(so.discarded)))
    {
        while (so.w != NIL) {
            /* w is ordered furthest-first, so llast(w) is the closest candidate */
            HnswSearchCandidate *sc = (HnswSearchCandidate *) llast(so.w);
            so.w = list_delete_last(so.w);

            HnswElement elem = HnswPtrAccess(base, sc->element);
            if (elem == NULL)
                continue;

            int n_tids = (int) elem->heaptidsLength;
            for (int i = 0; i < n_tids; i++) {
                av_docid docid = av_tid_to_docid(&elem->heaptids[i]);

                if (cand != NULL && !roaring_bitmap_contains(cand, docid))
                    continue;

                if (so.iterative_scan == HNSW_ITERATIVE_SCAN_STRICT) {
                    if (sc->distance < so.previous_distance)
                        continue;
                    so.previous_distance = sc->distance;
                } else {
                    so.previous_distance = sc->distance;
                }

                if (!acc_add(&acc, docid, sc->distance)) {
                    HnswEndSearchState(&so.searchState);
                    MemoryContextSwitchTo(oldCtx);
                    MemoryContextDelete(tmpCtx);
                    free(acc.items);
                    av_err_set(e, "internal", "out of memory collecting hnsw hits");
                    return AV_ERR;
                }
            }
        }

        /* Check if we have collected enough candidates */
        if (acc.count >= target_count) {
            if (so.iterative_scan == HNSW_ITERATIVE_SCAN_RELAXED && !done_extra_batch && so.discarded != NULL && !pairingheap_is_empty(so.discarded)) {
                /* Relaxed order does one extra batch after k */
                done_extra_batch = true;
                if (!readonly) {
                    LWLockAcquire(&graph->entryWaitLock, LW_EXCLUSIVE);
                    LWLockRelease(&graph->entryWaitLock);
                    LWLockAcquire(&graph->entryLock, LW_SHARED);
                }
                so.w = ResumeScanItemsMem(&so);
                if (!readonly) {
                    LWLockRelease(&graph->entryLock);
                }
                continue;
            }
            break;
        }

        if (so.iterative_scan == HNSW_ITERATIVE_SCAN_OFF)
            break;

        if (so.discarded == NULL || pairingheap_is_empty(so.discarded))
            break;

        /* Check resource limits */
        if (so.tuples >= so.max_scan_tuples || MemoryContextMemAllocated(so.tmpCtx, false) > so.max_memory) {
            /* Drain remaining from discarded heap directly without further exploration */
            while (!pairingheap_is_empty(so.discarded) && acc.count < target_count) {
                HnswSearchCandidate *sc = HnswGetSearchCandidate(w_node, pairingheap_remove_first(so.discarded));
                HnswElement elem = HnswPtrAccess(base, sc->element);
                if (elem == NULL)
                    continue;
                for (int i = 0; i < (int) elem->heaptidsLength; i++) {
                    av_docid docid = av_tid_to_docid(&elem->heaptids[i]);
                    if (cand != NULL && !roaring_bitmap_contains(cand, docid))
                        continue;
                    if (so.iterative_scan == HNSW_ITERATIVE_SCAN_STRICT) {
                        if (sc->distance < so.previous_distance)
                            continue;
                        so.previous_distance = sc->distance;
                    } else {
                        so.previous_distance = sc->distance;
                    }
                    if (!acc_add(&acc, docid, sc->distance))
                        break;
                }
            }
            break;
        }

        /* Resume search on Layer 0 */
        if (!readonly) {
            LWLockAcquire(&graph->entryWaitLock, LW_EXCLUSIVE);
            LWLockRelease(&graph->entryWaitLock);
            LWLockAcquire(&graph->entryLock, LW_SHARED);
        }
        so.w = ResumeScanItemsMem(&so);
        if (!readonly) {
            LWLockRelease(&graph->entryLock);
        }
        if (so.w == NIL)
            break;
    }

    HnswEndSearchState(&so.searchState);

    /* Sort accumulated hits by ascending (distance, docid) */
    if (acc.count > 1) {
        qsort(acc.items, acc.count, sizeof(av_hit), hit_cmp);
    }

    size_t count = 0;
    if (acc.count > offset) {
        size_t available = acc.count - offset;
        count = available < k ? available : k;
        memcpy(out, acc.items + offset, count * sizeof(av_hit));
    }

    *n_out = count;

    free(acc.items);
    MemoryContextSwitchTo(oldCtx);
    MemoryContextDelete(tmpCtx);
    return AV_OK;
}
