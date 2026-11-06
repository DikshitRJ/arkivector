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

typedef struct {
    av_segment *new_seg;
    const av_segment *old_seg;
    av_err *e;
    bool ok;
} compact_insert_ctx;

static bool compact_insert_cb(uint32_t docid, void *param)
{
    compact_insert_ctx *ctx = (compact_insert_ctx *) param;
    const void *v = av_segment_get(ctx->old_seg, (av_docid) docid);
    if (v != NULL) {
        if (av_segment_insert(ctx->new_seg, (av_docid) docid, v, ctx->e) != AV_OK) {
            ctx->ok = false;
            return false;
        }
    }
    return true;
}

av_status av_compact_rebuild(av_collection *col, const av_compact_config *cfg, av_err *e)
{
    if (col == NULL) {
        av_err_set(e, "invalid_argument", "collection required for compaction");
        return AV_ERR;
    }

    size_t live_count = av_collection_live_count(col);
    if (live_count == 0)
        return AV_OK;

    const av_coldef *def = av_collection_def(col);
    if (def == NULL) {
        av_err_set(e, "internal", "collection has no definition");
        return AV_ERR;
    }

    (void) cfg; /* expiry bands / parallel inserters not implemented yet: single-segment rebuild */

    av_collection_read_lock(col);
    const av_segment *old_seg = av_collection_segment(col);
    const roaring_bitmap_t *live = av_collection_live(col);
    if (old_seg == NULL || live == NULL) {
        av_collection_read_unlock(col);
        return AV_OK;
    }

    av_segment *new_seg = NULL;
    av_status st = AV_OK;
    if (def->method == AV_INDEX_HNSW) {
        int m = def->m > 0 ? def->m : 16;
        int ef_c = def->ef_construction > 0 ? def->ef_construction : 64;
        st = av_segment_create_hnsw(NULL, def->vtype, (int) def->dimensions, def->opclass, m, ef_c, 0,
                                    &new_seg, e);
    } else {
        st = av_segment_create_flat(NULL, def->vtype, (int) def->dimensions, 0, &new_seg, e);
    }

    if (st != AV_OK || new_seg == NULL) {
        av_collection_read_unlock(col);
        return st;
    }

    compact_insert_ctx ctx = {new_seg, old_seg, e, true};
    roaring_iterate(live, compact_insert_cb, &ctx);
    av_collection_read_unlock(col);

    if (!ctx.ok) {
        av_segment_destroy(new_seg);
        return AV_ERR;
    }

    if (av_segment_kind(new_seg) == AV_SEG_HNSW) {
        st = av_segment_seal(new_seg, e);
        if (st != AV_OK) {
            av_segment_destroy(new_seg);
            return st;
        }
    }

    return av_collection_swap_segment(col, new_seg, e);
}
