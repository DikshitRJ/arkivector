/*
 * query/executor.h: query execution and hit formatting (D3).
 */
#ifndef AV_QUERY_EXECUTOR_H
#define AV_QUERY_EXECUTOR_H

#include <roaring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

#include "core/collection.h"
#include "kernels/opclass.h"
#include "query/planner.h"
#include "store/lmdb_store.h"

#define AV_INCLUDE_ATTRIBUTES (1 << 0)
#define AV_INCLUDE_DOCUMENT   (1 << 1)
#define AV_INCLUDE_VECTOR     (1 << 2)

typedef struct {
    const void *query_vec; /* decoded pgvector struct */
    const av_opclass *oc;  /* opclass matching operator */
    size_t k;              /* 1..1000 */
    size_t offset;         /* offset + k <= 1000 */
    bool has_max_distance;
    double max_distance;
    bool has_min_score;
    double min_score;
    bool exact;                 /* force exact */
    int include_flags;          /* AV_INCLUDE_* */
    int ef_search;              /* 1..1000 */
    const char *iterative_scan; /* "off" | "relaxed_order" | "strict_order" */
    size_t max_scan_tuples;
    bool explain;
} av_search_params;

typedef struct {
    uint8_t key[32];
    char id[129];
    bool has_id;
    uint8_t owner[20];
    uint8_t creator[20];
    uint64_t created_at;
    uint64_t updated_at;
    uint64_t expires_at;
    double distance;
    double score;
    char *document;
    size_t doc_len;
    char *vector_bytes; /* base64 string */
    yyjson_mut_val *attributes_obj;
} av_search_hit;

typedef struct {
    uint64_t as_of_block;
    const char *sync_state;
    bool open_collection;
    av_search_hit *hits;
    size_t n_hits;
    av_query_plan plan;
    bool explain;
    yyjson_mut_doc *doc;
} av_search_result;

av_status av_execute_search(av_collection *col, av_store *store, const av_search_params *params,
                            const roaring_bitmap_t *cand, av_search_result *out, av_err *e);

void av_search_result_free(av_search_result *res);

#endif /* AV_QUERY_EXECUTOR_H */
