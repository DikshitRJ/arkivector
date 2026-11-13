/*
 * index/hnsw_index.h: the HNSW side index of one collection (GitHub issue #11).
 *
 * Expiry bands. Entities are grouped by expiry block: band b holds expires_at in
 * [b * band_blocks, (b + 1) * band_blocks) (band_blocks 0 = one band for everything; expires_at 0 =
 * the no-expiry band). Each band has its own pgvector HNSW graph plus an exact "tail" (docids the
 * graph could not take). Once a band's last block has passed, the whole band (graph, tail, file) is
 * released at once: no tombstone work. An extended entity is inserted into its new band; its old
 * element stays behind as a dead entry until the old band is dropped. Searches query every band
 * and the collection merges the top-k (av_collection_search_hnsw).
 *
 * Background builds. Builds and rebuilds run on the index's worker thread, never on the libuv loop
 * or the sync (applier) thread. The applier snapshots the band's live docids, the worker builds a
 * fresh graph from the flat segment, and the applier adopts it after a publish: it inserts the
 * docids that arrived since the snapshot, then swaps the band's graph pointer under the index lock.
 * Until then searches use the old graph plus the exact tail. A band that has no graph yet (first
 * start without a file) is searched exactly.
 *
 * Persistence. With a directory, each band's graph is written to <dir>/<band>.hnsw (header with
 * the build parameters, opclass, pgvector tag, docid bound and block watermark, CRC-32 of header
 * and image) after every background build, at checkpoints (worker thread, from a snapshot taken
 * under the shared lock) and at shutdown. At startup av_hnsw_index_open loads each file, validates
 * it completely (av_segment_hnsw_restore), checks every live docid against the flat segment, and
 * only indexes the docids added since the save. A missing, corrupt or mismatched file logs a
 * warning and falls back to a background rebuild; it never crashes the daemon.
 *
 * Threads and locks:
 *  - lock (rwlock, writer-preferring) guards the band array and each band's graph, tail, members
 *    and flags. Readers (query workers) hold it shared for one search; the applier holds it
 *    exclusively per insert, swap and drop; the worker holds it shared while it copies a graph
 *    for a checkpoint. Lock order: collection read lock, then this lock.
 *  - mu/cv guard the job queues (pending jobs for the worker, finished builds for the applier).
 *  - av_hnsw_index_add / _expiry_changed / _after_publish: applier thread only.
 *  - av_hnsw_index_search / _describe: any thread.
 *  - av_hnsw_index_close: once, when no other thread uses the index any more.
 * Ownership: the index borrows `flat` (the collection's flat segment), which must outlive it.
 */
#ifndef AV_INDEX_HNSW_INDEX_H
#define AV_INDEX_HNSW_INDEX_H

#include <roaring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/vtype.h"
#include "index/docid.h"
#include "index/segment.h"
#include "kernels/opclass.h"
#include "log.h"

typedef struct av_hnsw_index av_hnsw_index;

#define AV_HNSW_BAND_NONE    UINT64_MAX /* band of entities without expiry */
#define AV_HNSW_FILE_MAGIC   "AVHNSW01"
#define AV_HNSW_FILE_VERSION 1u
#define AV_HNSW_FILE_HEADER  256u

typedef struct {
    const char *coll_name;
    av_vtype vtype;
    int dim;
    const av_opclass *oc;
    int m;
    int ef_construction;
    const av_segment *flat;
    const char *dir;      /* persistence directory, created if missing; NULL = in memory only */
    uint64_t band_blocks; /* 0 = a single band */
} av_hnsw_index_config;

/* Expiry block of a live docid (applier-side view at open). */
typedef uint64_t (*av_hnsw_expiry_fn)(void *ctx, av_docid id);

av_hnsw_index *av_hnsw_index_create(const av_hnsw_index_config *cfg, av_err *e);

/*
 * Startup (before the collection is shared): partition live docids into bands, load each band's
 * file when valid (verify against flat, then index only the missing docids, or schedule a rebuild
 * when that delta is large), else start a background build. next_docid is the store's committed
 * docid bound (a file whose docids reach it is rejected). Files of expired or unknown bands are
 * removed. Never fails on file problems; AV_ERR only on out of memory.
 */
av_status av_hnsw_index_open(av_hnsw_index *ix, const roaring_bitmap_t *live, av_hnsw_expiry_fn expiry,
                             void *ctx, uint32_t next_docid, uint64_t as_of_block, av_err *e);

/* Applier: index a docid that was just stored in the flat segment. Never fails the batch. */
void av_hnsw_index_add(av_hnsw_index *ix, av_docid id, const void *pg, uint64_t expires_at);

/* Applier: the expiry of a kept docid changed (extend, patch with ttl). Moves it to its new band. */
void av_hnsw_index_expiry_changed(av_hnsw_index *ix, av_docid id, uint64_t old_expires, uint64_t new_expires);

/*
 * Applier, after each publish at `block` (live = the published live bitmap, next_docid = the
 * committed bound): release expired bands, adopt finished background builds (catch-up, swap),
 * schedule rebuilds of bands with a large exact tail or dead share, and schedule checkpoints.
 */
void av_hnsw_index_after_publish(av_hnsw_index *ix, const roaring_bitmap_t *live, uint64_t block,
                                 uint32_t next_docid);

/* Any thread: ask for a background rebuild of every band; the applier schedules it at its next
 * publish (bands already building are skipped). */
void av_hnsw_index_request_rebuild(av_hnsw_index *ix);

/*
 * Candidate docids for an approximate search: the graph hits of every band (limit each, ef_search,
 * iterative mode, max_scan_tuples as for av_segment_search_hnsw) plus the exact tails, all
 * restricted to cand. Appends to *ids (realloc'd, caller frees); duplicates are possible.
 */
av_status av_hnsw_index_search(av_hnsw_index *ix, const void *query, const roaring_bitmap_t *cand,
                               int ef_search, int iterative_mode, int max_scan_tuples, size_t limit,
                               av_docid **ids, size_t *n_ids, size_t *cap_ids, av_err *e);

typedef struct {
    uint64_t band;
    size_t graph_count; /* docids in the graph (live or dead) */
    size_t tail_count;  /* docids scanned exactly */
    const char *source; /* "loaded", "built", "live" (incremental only) or "pending" (no graph yet) */
    bool building;
} av_hnsw_band_info;

typedef struct {
    size_t bands;
    uint64_t loads;         /* band files loaded at startup */
    uint64_t load_failures; /* band files rejected (corrupt, mismatched) */
    uint64_t builds;        /* background builds adopted */
    uint64_t builds_running;
    uint64_t bands_dropped; /* expired bands released */
    uint64_t saves;         /* band files written */
} av_hnsw_index_stats;

/* Fill up to max band rows (sorted by band) and the counters. Returns the number of bands. */
size_t av_hnsw_index_describe(av_hnsw_index *ix, av_hnsw_band_info *out, size_t max, av_hnsw_index_stats *st);

/* Number of bands (any thread). */
size_t av_hnsw_index_band_count(av_hnsw_index *ix);

/*
 * Stop the worker (an unfinished build is abandoned), then, when persist is set, write every band
 * changed since its last save, and free everything. remove_files deletes the band files instead
 * (collection dropped).
 */
void av_hnsw_index_close(av_hnsw_index *ix, bool persist, bool remove_files, uint32_t next_docid,
                         uint64_t as_of_block);

/* ---- file format (exposed for tests and the fuzz target) ---- */

typedef struct {
    av_vtype vtype;
    int dim;
    int m;
    int ef_construction;
    const char *opclass;
    uint64_t band;
    uint64_t band_blocks;
    uint32_t next_docid; /* reject images with docids >= this */
} av_hnsw_file_expect;

typedef struct {
    uint64_t band;
    uint64_t watermark_block;
    uint32_t next_docid_at_save;
    size_t count;
} av_hnsw_file_info;

/* Serialize a graph image (av_segment_hnsw_snapshot) with its header into a malloc'd buffer. */
av_status av_hnsw_file_encode(const void *img, size_t img_len, const av_seg_hnsw_info *info,
                              const char *opclass, uint64_t band, uint64_t band_blocks, uint32_t next_docid,
                              uint64_t watermark, uint8_t **out, size_t *out_len, av_err *e);

/*
 * Parse an untrusted band file: header checks (magic, version, sizes, CRC-32 unless check_crc is
 * false, parameters equal to expect), then av_segment_hnsw_restore. On success *out is a writable
 * graph segment. Fuzzed by fuzz_hnsw_file.
 */
av_status av_hnsw_file_decode(const uint8_t *buf, size_t len, const av_hnsw_file_expect *expect,
                              const av_opclass *oc, bool check_crc, size_t reserve_bytes, av_segment **out,
                              av_hnsw_file_info *info, av_err *e);

uint32_t av_crc32(uint32_t crc, const void *buf, size_t len);

#endif /* AV_INDEX_HNSW_INDEX_H */
