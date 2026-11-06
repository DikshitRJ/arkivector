/*
 * query/planner.h: query planner (D3: exact flat; D4 adds HNSW and cost model).
 */
#ifndef AV_QUERY_PLANNER_H
#define AV_QUERY_PLANNER_H

#include <roaring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/collection.h"
#include "kernels/dispatch.h"
#include "kernels/opclass.h"
#include "log.h"

typedef enum {
    AV_STRAT_EXACT_FLAT = 1,
    AV_STRAT_HNSW = 2,
    AV_STRAT_HNSW_ITERATIVE = 3,
    AV_STRAT_MIXED = 4,
    AV_STRAT_REMOTE_PREFILTER = 5,
    AV_STRAT_REMOTE_POSTFILTER = 6
} av_plan_strategy;

const char *av_plan_strategy_name(av_plan_strategy s);

typedef struct {
    av_plan_strategy strategy;
    bool exact;
    size_t candidates;
    double selectivity;
    const char *kernel;
    double elapsed_ms;
} av_query_plan;

/*
 * In D3, always plans AV_STRAT_EXACT_FLAT with exact = true.
 */
av_status av_plan_query(const av_collection *col, const void *query, const av_opclass *oc, bool force_exact,
                        const roaring_bitmap_t *cand, size_t live_total, av_query_plan *out, av_err *e);

#endif /* AV_QUERY_PLANNER_H */
