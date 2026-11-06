/*
 * index/segment.h: vector segments (PLAN_DAEMON section 3). D2 implements AV_SEG_FLAT:
 * decoded pgvector structs stored in a stable-address arena_file at 8-byte alignment plus
 * a docid -> offset table. Tombstones are not tracked here; liveness comes from the
 * collection's live bitmap. AV_SEG_HNSW arrives in D4 behind the same API.
 *
 * Thread safety: av_segment_insert is for ONE writer thread (the applier). av_segment_get,
 * av_segment_count and av_segment_scan_exact are read-only and safe from any thread for
 * docids that were published to the reader by a synchronising operation (the collection's
 * publish lock). The data region and the offset table never move once committed, and
 * inserted structs are immutable, so returned pointers stay valid until av_segment_destroy.
 */
#ifndef AV_INDEX_SEGMENT_H
#define AV_INDEX_SEGMENT_H

#include <roaring.h>
#include <stdbool.h>
#include <stddef.h>

#include "codec/vtype.h"
#include "index/docid.h"
#include "kernels/opclass.h"
#include "log.h"

typedef enum { AV_SEG_FLAT = 1, AV_SEG_HNSW = 2 } av_seg_kind;
typedef struct av_segment av_segment;

#define AV_SEG_DEFAULT_RESERVE ((size_t) 64 << 30)

/*
 * Create an empty flat segment for one column type/dimension. path names the backing file
 * (truncated), NULL keeps it in anonymous memory. reserve_bytes bounds the vector data
 * (0 = AV_SEG_DEFAULT_RESERVE, reduced automatically if the address space refuses it).
 */
av_status av_segment_create_flat(const char *path, av_vtype type, int dim, size_t reserve_bytes,
                                 av_segment **out, av_err *e);
void av_segment_destroy(av_segment *s);
av_seg_kind av_segment_kind(const av_segment *s);

/* Copy a pgvector struct (varlena header set) in as docid id. Duplicate ids are rejected. */
av_status av_segment_insert(av_segment *s, av_docid id, const void *pgvec, av_err *e);
size_t av_segment_count(const av_segment *s);
/* The stored struct, or NULL when id is not in this segment. */
const void *av_segment_get(const av_segment *s, av_docid id);

typedef struct {
    av_docid id;
    double distance;
} av_hit;

/*
 * Exact scan. cand is the candidate set to score; NULL scores every docid stored in the
 * segment. The segment knows nothing about liveness, so callers pass (filter AND live)
 * (see av_collection_scan_exact). Keeps a bounded max-heap of k and returns the best k
 * ascending by (distance, docid); NaN distances sort last. out must hold k entries.
 * Errors: dimension_mismatch (query dim), invalid_argument (opclass type).
 */
av_status av_segment_scan_exact(const av_segment *s, const void *query, const av_opclass *oc,
                                const roaring_bitmap_t *cand, size_t k, av_hit *out, size_t *n_out, av_err *e);

#endif /* AV_INDEX_SEGMENT_H */
