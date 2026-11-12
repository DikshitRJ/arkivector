/*
 * index/compact.c: Background compactor and rebuild trigger.
 */
#include "index/compact.h"

#include <stdlib.h>
#include <string.h>

void av_compact_config_init(av_compact_config *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->dead_ratio_trigger = 0.30;
    cfg->small_segment_threshold = 1000;
    cfg->band_blocks = 302400; /* ~7 days @ 2s blocks */
    cfg->max_segments = 16;
    cfg->parallel_workers = 4;
}

bool av_compact_should_rebuild(size_t live_count, size_t total_count, size_t n_segments,
                               const size_t *seg_counts, const av_compact_config *cfg)
{
    av_compact_config def;
    if (cfg == NULL) {
        av_compact_config_init(&def);
        cfg = &def;
    }

    if (total_count > 0 && total_count >= cfg->small_segment_threshold) {
        size_t dead = total_count > live_count ? (total_count - live_count) : 0;
        double ratio = (double) dead / (double) total_count;
        if (ratio > cfg->dead_ratio_trigger)
            return true;
    }

    if (n_segments > cfg->max_segments)
        return true;

    if (seg_counts != NULL && n_segments >= 2) {
        size_t n_small = 0;
        for (size_t i = 0; i < n_segments; i++) {
            if (seg_counts[i] < cfg->small_segment_threshold) {
                n_small++;
                if (n_small >= 2)
                    return true;
            }
        }
    }

    return false;
}

av_status av_compact_rebuild(av_collection *col, const av_compact_config *cfg, av_err *e)
{
    if (col == NULL) {
        av_err_set(e, "invalid_argument", "collection required for compaction");
        return AV_ERR;
    }
    (void) cfg; /* expiry bands / parallel inserters not implemented yet: single-graph rebuild */
    /*
     * The flat segment is the source of truth and is never replaced; compaction rebuilds the HNSW
     * graph from the live docids (dropping dead elements and the exact tail). Applier thread only.
     * Collections without an hnsw index have nothing to rebuild.
     */
    return av_collection_rebuild_hnsw(col, e);
}
