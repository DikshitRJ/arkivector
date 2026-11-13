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
#include <stdint.h>

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

/* Address-space reservation (bytes) for an HNSW segment holding about max_vectors vectors
 * (virtual: pages are committed on use). Capped at AV_SEG_DEFAULT_RESERVE. */
size_t av_segment_hnsw_reserve(av_vtype type, int dim, int m, size_t max_vectors);

/* Open an existing sealed HNSW segment (read-only mmap with footer validation). */
av_status av_segment_open_sealed(const char *path, av_segment **out, av_err *e);

/* Seal an HNSW segment, writing the footer and msyncing. */
av_status av_segment_seal(av_segment *s, av_err *e);
bool av_segment_is_sealed(const av_segment *s);

void av_segment_destroy(av_segment *s);
av_seg_kind av_segment_kind(const av_segment *s);

/* Copy a pgvector struct (varlena header set) in as docid id. Duplicate ids are rejected.
 * For HNSW segments, returns AV_SEAL_NEEDED when the arena reservation memory is full and
 * AV_SKIPPED for a zero-norm vector under a normalizing opclass (pgvector does not index those). */
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
 * over the in-memory/sealed graph, rechecking cand. Distances are the INDEX distances
 * (l2: squared, ip: negative, cosine: on normalized values); callers that report distances
 * recompute them with the operator (av_collection_search_hnsw). A zero-norm query under a
 * normalizing opclass returns no hits. pgvector errors are caught and returned as AV_ERR.
 */
av_status av_segment_search_hnsw(const av_segment *s, const void *query, int ef_search, int iterative_mode,
                                 int max_scan_tuples, const roaring_bitmap_t *cand, size_t offset, size_t k,
                                 av_hit *out, size_t *n_out, av_err *e);

/* ---- HNSW persistence (GitHub issue #11): snapshot / validated restore of a graph arena ---- */

typedef struct {
    av_vtype type;
    int dim;
    int m;
    int ef_construction;
    size_t count;       /* elements (one docid each) */
    size_t arena_bytes; /* bytes of the arena image, HnswGraph at offset 0 */
    uint32_t min_docid;
    uint32_t max_docid;
} av_seg_hnsw_info;

/*
 * Copy of an HNSW segment's arena [0, used) and its parameters. The caller keeps the graph from
 * changing for the duration (the HNSW index lock, shared). *bytes is malloc'd (caller frees).
 */
av_status av_segment_hnsw_snapshot(const av_segment *s, void **bytes, size_t *len, av_seg_hnsw_info *info,
                                   av_err *e);

/*
 * Writable HNSW segment from an arena image read from disk. The image is UNTRUSTED: every relative
 * pointer is bounds- and alignment-checked, every element is reachable once from the head list,
 * levels, neighbour counts and neighbour targets are valid (a neighbour at layer l has level >= l),
 * values have the exact pgvector layout for (type, dim), docids are unique and <= expect->max_docid,
 * and count / entry point match. Locks in the image are reset. expect gives the collection's type,
 * dim, m, ef_construction and opclass, the element count and the docid bound. reserve_bytes is the
 * address space for growth (at least len). Fuzzed by fuzz_hnsw_file.
 */
av_status av_segment_hnsw_restore(const void *bytes, size_t len, const av_seg_hnsw_info *expect,
                                  const av_opclass *oc, size_t reserve_bytes, av_segment **out, av_err *e);

/*
 * Check that every docid of graph g that is in live holds the index form (normalized for cosine)
 * of the raw vector of the same docid in flat. A mismatch means the image does not belong to this
 * store (docids reassigned): *mismatches counts them. AV_ERR only on pgvector errors.
 */
av_status av_segment_hnsw_verify(const av_segment *g, const av_segment *flat, const roaring_bitmap_t *live,
                                 size_t *mismatches, av_err *e);

/* Docids stored in a segment (caller frees). NULL on out of memory. */
roaring_bitmap_t *av_segment_docids(const av_segment *s);

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
