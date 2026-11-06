/*
 * query/planner.c: query planner implementation.
 */
#include "query/planner.h"

#include <string.h>

const char *av_plan_strategy_name(av_plan_strategy s)
{
    switch (s) {
    case AV_STRAT_EXACT_FLAT:
        return "exact_flat";
    case AV_STRAT_HNSW:
        return "hnsw";
    case AV_STRAT_HNSW_ITERATIVE:
        return "hnsw_iterative";
    case AV_STRAT_MIXED:
        return "mixed";
    case AV_STRAT_REMOTE_PREFILTER:
        return "remote_prefilter";
    case AV_STRAT_REMOTE_POSTFILTER:
        return "remote_postfilter";
    }
    return "exact_flat";
}

av_status av_plan_query(const av_collection *col, const void *query, const av_opclass *oc,
                        bool force_exact, const roaring_bitmap_t *cand, size_t live_total,
                        av_query_plan *out, av_err *e)
{
    (void) col;
    (void) query;
    (void) oc;
    (void) force_exact;
    (void) e;

    memset(out, 0, sizeof(*out));
    out->strategy = AV_STRAT_EXACT_FLAT;
    out->exact = true;
    out->candidates = cand != NULL ? (size_t) roaring_bitmap_get_cardinality(cand) : live_total;
    out->selectivity = live_total > 0 ? (double) out->candidates / (double) live_total : 1.0;
    out->kernel = av_kernel_tier();
    out->elapsed_ms = 0.0;
    return AV_OK;
}
