/*
 * query/planner.c: query planner implementation.
 */
#include "query/planner.h"

#include <math.h>
#include <string.h>

#include "query/calibrate.h"

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
    (void) query;
    (void) e;

    memset(out, 0, sizeof(*out));
    out->candidates = cand != NULL ? (size_t) roaring_bitmap_get_cardinality(cand) : live_total;
    out->selectivity = live_total > 0 ? (double) out->candidates / (double) live_total : 1.0;
    out->kernel = av_kernel_tier();
    out->elapsed_ms = 0.0;

    const av_segment *seg = col != NULL ? av_collection_segment(col) : NULL;
    if (seg == NULL || av_segment_kind(seg) != AV_SEG_HNSW) {
        out->strategy = AV_STRAT_EXACT_FLAT;
        out->exact = true;
        return AV_OK;
    }

    /* Guards for forcing flat exact scan */
    if (force_exact || out->candidates <= 2048 || out->selectivity < 0.005) {
        out->strategy = AV_STRAT_EXACT_FLAT;
        out->exact = true;
        return AV_OK;
    }

    const av_coldef *def = av_collection_def(col);
    if (oc != NULL && def != NULL && def->opclass != NULL && strcmp(oc->op, def->opclass->op) != 0) {
        out->strategy = AV_STRAT_EXACT_FLAT;
        out->exact = true;
        return AV_OK;
    }

    /* Crossover threshold: T = gamma * sqrt(m * ef * N) */
    const av_calibration *cal = av_calibration_get();
    double gamma = cal->gamma > 0.0 ? cal->gamma : 1.7;
    int m = (def != NULL && def->m > 0) ? def->m : 16;
    int ef = 40;
    double t_thresh = gamma * sqrt((double) m * (double) ef * (double) live_total);

    if ((double) out->candidates < t_thresh) {
        out->strategy = AV_STRAT_EXACT_FLAT;
        out->exact = true;
    } else {
        out->strategy = (cand == NULL) ? AV_STRAT_HNSW : AV_STRAT_HNSW_ITERATIVE;
        out->exact = false;
    }

    return AV_OK;
}
