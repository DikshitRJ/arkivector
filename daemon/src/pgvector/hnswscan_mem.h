/*
 * pgvector/hnswscan_mem.h: In-memory HNSW scan over arena graphs using pgvector's
 * GetScanItems and ResumeScanItems.
 */
#ifndef AV_PGVECTOR_HNSWSCAN_MEM_H
#define AV_PGVECTOR_HNSWSCAN_MEM_H

#include "postgres.h"
#include <roaring.h>
#include "hnsw.h"
#include "index/segment.h"
#include "log.h"

typedef struct AvHnswScanState {
    char *base;
    HnswGraph *graph;
    HnswSupport support;
    HnswQuery q;
    int m;
    int ef_search;
    int iterative_scan;
    int max_scan_tuples;
    Size max_memory;
    int64 tuples;
    double previous_distance;
    visited_hash v;
    pairingheap *discarded;
    List *w;
    HnswSearchState searchState;
    MemoryContext tmpCtx;
} AvHnswScanState;

av_status av_hnsw_scan_mem(char *base, HnswGraph *graph, HnswSupport *support, int m,
                           Datum query_val, int ef_search, int iterative_mode,
                           int max_scan_tuples, const roaring_bitmap_t *cand,
                           size_t offset, size_t k, av_hit *out, size_t *n_out,
                           av_err *e);

#endif /* AV_PGVECTOR_HNSWSCAN_MEM_H */
