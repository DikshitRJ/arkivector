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

/*
 * Create an empty HNSW segment backed by an arena_file.
 */
av_status av_segment_create_hnsw(const char *path, av_vtype type, int dim, const av_opclass *oc, int m,
                                 int ef_construction, size_t reserve_bytes, av_segment **out, av_err *e);

/* Open an existing sealed HNSW segment (read-only mmap with footer validation). */
av_status av_segment_open_sealed(const char *path, av_segment **out, av_err *e);

/* Seal an HNSW segment, writing the footer and msyncing. */
av_status av_segment_seal(av_segment *s, av_err *e);
bool av_segment_is_sealed(const av_segment *s);

void av_segment_destroy(av_segment *s);
av_seg_kind av_segment_kind(const av_segment *s);

/* Copy a pgvector struct (varlena header set) in as docid id. Duplicate ids are rejected.
 * For HNSW segments, returns AV_SEAL_NEEDED when the arena reservation memory is full. */
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
                                const roaring_bitmap_t *cand, size_t k, av_hit *out, size_t *n_out,
                                av_err *e);

/*
 * Approximate HNSW search. Runs GetScanItems (and ResumeScanItems if iterative != off)
 * over the in-memory/sealed graph, rechecking cand.
 */
av_status av_segment_search_hnsw(const av_segment *s, const void *query, int ef_search, int iterative_mode,
                                 int max_scan_tuples, const roaring_bitmap_t *cand, size_t offset, size_t k,
                                 av_hit *out, size_t *n_out, av_err *e);

#define AV_SEG_MAGIC "AVSEG1"

typedef struct {
    char magic[8];         /* "AVSEG1\0\0" */
    char pgvector_tag[32]; /* AV_PGVECTOR_TAG */
    char opclass_name[32]; /* opclass->name */
    uint32_t m;
    uint32_t ef_construction;
    uint32_t dimensions;
    uint32_t vtype;              /* av_vtype */
    uint64_t entry_point_offset; /* relptr offset to entryPoint */
    uint64_t count;              /* number of elements/vectors */
    uint32_t min_docid;
    uint32_t max_docid;
    uint64_t prng_seed;
    uint64_t arena_bytes; /* size of arena data up to footer */
    uint32_t footer_crc;
} av_seg_footer;

#endif /* AV_INDEX_SEGMENT_H */
