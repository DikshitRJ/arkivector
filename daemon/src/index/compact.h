/*
 * index/compact.h: Background compactor and rebuild trigger.
 */
#ifndef AV_INDEX_COMPACT_H
#define AV_INDEX_COMPACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/collection.h"
#include "index/segment.h"
#include "log.h"

typedef struct {
    double dead_ratio_trigger;      /* default 0.30 (30%) */
    size_t small_segment_threshold; /* default 1000 */
    uint64_t band_blocks;           /* default 302,400 (~7 days @ 2s blocks) */
    size_t max_segments;            /* default 16 */
    size_t parallel_workers;        /* default 4 */
} av_compact_config;

void av_compact_config_init(av_compact_config *cfg);

/* Returns true if dead ratio > trigger or small-segment count >= 2 */
bool av_compact_should_rebuild(size_t live_count, size_t total_count, size_t n_segments,
                               const size_t *seg_counts, const av_compact_config *cfg);

/* Rebuilds collection segments with live vectors partitioned into expiry bands */
av_status av_compact_rebuild(av_collection *col, const av_compact_config *cfg, av_err *e);

#endif /* AV_INDEX_COMPACT_H */
