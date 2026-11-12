/*
 * core/collection.h: the runtime object of one collection: definition, LMDB-backed rows,
 * flat vector segment, live bitmap, attribute postings, docid maps, as_of block, publish
 * lock and the wait-for-block condition (PLAN_DAEMON section 3, ARCHITECTURE sections 8 and 9.6).
 *
 * Threads:
 *  - ONE applier thread calls av_collection_upsert/_delete/_extend/_transfer between
 *    av_store_begin and av_collection_publish (or av_collection_discard). These calls edit only
 *    applier-private state: LMDB rows (inside the txn), the segment (append-only), a working
 *    copy of the live bitmap, and a staged change list. Nothing a reader can see changes.
 *  - av_collection_publish(), called after av_store_commit, takes the publish rwlock
 *    exclusively, applies the staged docmap / attribute-index edits, swaps in the live bitmap,
 *    sets as_of_block and wakes av_collection_wait_block waiters (microseconds of work).
 *  - Readers (query workers) hold av_collection_read_lock for the whole query. Everything the
 *    accessors below return (docmap, attr index, live bitmap, segment) is stable for exactly
 *    that duration and shows one consistent block: until publish a reader sees the old state.
 *  - Direct LMDB reads (av_store_get_entity) are NOT snapshot-consistent with publish: a row
 *    may already reflect a committed but not yet published batch. Resolve keys through the
 *    docmap and check liveness first.
 *  - If any applier call returns AV_ERR, the batch is poisoned: av_store_abort the txn and
 *    call av_collection_discard().
 * Lifetime: reference counted. av_collection_create returns one reference; the registry and
 * every user that keeps the pointer across calls takes its own (av_collection_retain).
 */
#ifndef AV_CORE_COLLECTION_H
#define AV_CORE_COLLECTION_H

#include <roaring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"
#include "core/coldef.h"
#include "index/attr_index.h"
#include "index/docid.h"
#include "index/segment.h"
#include "log.h"
#include "store/lmdb_store.h"

typedef struct av_collection av_collection;

typedef struct {
    size_t segment_reserve; /* flat segment address-space reservation (0 = default) */
} av_collection_opts;

/*
 * Build the runtime object. def is deep-copied. data_dir (may be NULL = anonymous memory)
 * receives the flat segment file "<name>.flat" (rebuilt on every start). When the store
 * already holds rows for the collection, docid maps, live bitmap, postings and the flat
 * segment are rebuilt from them (store/checkpoint.c) and as_of_block is taken from the
 * stored cursor. The collection's LMDB databases must exist (av_store_put_collection).
 */
av_collection *av_collection_create(const av_coldef *def, av_store *store, const char *data_dir, av_err *e);
av_collection *av_collection_create_opts(const av_coldef *def, av_store *store, const char *data_dir,
                                         const av_collection_opts *opts, av_err *e);
void av_collection_destroy(av_collection *c); /* same as release */
void av_collection_retain(av_collection *c);
void av_collection_release(av_collection *c);
/* The last reference removes the segment file as well (collection deleted). */
void av_collection_mark_dropped(av_collection *c);

const av_coldef *av_collection_def(const av_collection *c);
uint64_t av_collection_as_of(const av_collection *c);
bool av_collection_is_bootstrapping(const av_collection *c);
void av_collection_set_bootstrapping(av_collection *c, bool b);
bool av_collection_has_key(const av_collection *c, const uint8_t key[32]);
void av_collection_read_lock(av_collection *c);
void av_collection_read_unlock(av_collection *c);
/* Block until as_of_block >= min_block; consistency_timeout (e->as_of_block set) after timeout_ms. */
av_status av_collection_wait_block(av_collection *c, uint64_t min_block, int timeout_ms, av_err *e);

/* ---- applier-side mutation API (single thread), applied between begin and publish ---- */

typedef enum {
    AV_UP_STALE,          /* updated_at older than the stored state: ignored (monotonic rule) */
    AV_UP_NOT_MEMBER,     /* wrong schema tag or creator/owner not allowed: ignored */
    AV_UP_REMOVED,        /* no longer a member (or invalid) and was stored: removed */
    AV_UP_QUARANTINED,    /* malformed / mismatching av.vec: quarantined (and removed if stored) */
    AV_UP_UNCHANGED,      /* identical state: nothing written */
    AV_UP_CREATED,        /* new key: new docid */
    AV_UP_VECTOR_CHANGED, /* av.vec hash changed: new docid, old tombstoned */
    AV_UP_ATTRS_ONLY      /* docid kept, postings / row updated */
} av_upsert_outcome;

av_status av_collection_upsert_ex(av_collection *c, av_txn *t, const av_entity *ent,
                                  av_upsert_outcome *outcome, av_err *e);
/* *changed is true for CREATED / VECTOR_CHANGED / ATTRS_ONLY. */
av_status av_collection_upsert(av_collection *c, av_txn *t, const av_entity *ent, bool *changed, av_err *e);
/* Unknown keys are ignored (idempotent). Also clears the quarantine entry. */
av_status av_collection_delete(av_collection *c, av_txn *t, const uint8_t key[32], av_err *e);
av_status av_collection_extend(av_collection *c, av_txn *t, const uint8_t key[32], uint64_t expires_at,
                               av_err *e);
/* Reclaim up to budget entities whose expires_at <= block. budget 0 = default (10000). */
av_status av_collection_expire(av_collection *c, av_txn *t, uint64_t block, size_t budget, av_err *e);
/* Under membership=owner a transfer to a non-writer removes the entity. */
av_status av_collection_transfer(av_collection *c, av_txn *t, const uint8_t key[32],
                                 const uint8_t new_owner[20], av_err *e);
/* After av_store_commit: apply staged edits under the write lock, swap live, set as_of, broadcast. */
void av_collection_publish(av_collection *c, uint64_t block);
/* After av_store_abort (or an applier error): drop staged edits and the working live copy. */
void av_collection_discard(av_collection *c);

/* ---- reader side (call under av_collection_read_lock unless noted) ---- */

size_t av_collection_live_count(const av_collection *c); /* atomic; no lock needed */
const roaring_bitmap_t *av_collection_live(const av_collection *c);
const av_docmap *av_collection_docmap(const av_collection *c);
const av_attr_index *av_collection_attr_index(const av_collection *c);
const av_segment *av_collection_segment(const av_collection *c); /* the flat segment (raw vectors) */

/*
 * HNSW index (collections whose index method is hnsw). The graph indexes the docids of the flat
 * segment; docids it could not take (graph full, zero-norm cosine vectors) form an exact "tail".
 * The applier rebuilds the graph after publish when the tail or the dead share grows large.
 */
bool av_collection_has_hnsw(const av_collection *c);
/* Applier thread only: rebuild the graph from the published live docids now. */
av_status av_collection_rebuild_hnsw(av_collection *c, av_err *e);
/*
 * Approximate search under the read lock: graph search (pgvector GetScanItems/ResumeScanItems with
 * ef_search >= limit) over cand (NULL = live) plus an exact scan of the tail. Distances are
 * recomputed with oc (NULL = column opclass) on the raw vectors; out[0..*n_out) holds the best
 * limit hits ascending by (distance, key). Fewer than limit hits means the graph walk ran out.
 */
av_status av_collection_search_hnsw(av_collection *c, const void *query, const av_opclass *oc,
                                    const roaring_bitmap_t *cand, int ef_search, int iterative_mode,
                                    int max_scan_tuples, size_t limit, av_hit *out, size_t *n_out, av_err *e);

/*
 * Exact search: scores (filter AND live), or all live docids when filter is NULL. The caller
 * holds the read lock. Returns the best k ascending by (distance, docid) in out[0..*n_out).
 */
av_status av_collection_scan_exact(const av_collection *c, const void *query, const roaring_bitmap_t *filter,
                                   size_t k, av_hit *out, size_t *n_out, av_err *e);

typedef struct {
    size_t live;
    size_t tombstones; /* allocated docids that are not live */
    size_t quarantined;
    size_t segments;
} av_collection_stats;
void av_collection_get_stats(av_collection *c, av_collection_stats *out); /* takes the read lock itself */

/* ---- debug routes ---- */
typedef bool (*av_collection_key_cb)(const uint8_t key[32], uint64_t updated_at, uint64_t expires_at,
                                     void *ctx);
/* Takes the read lock itself (do not call while holding it). Sorted by key; false from cb stops. */
void av_collection_each_live(av_collection *c, av_collection_key_cb cb, void *ctx);

#endif /* AV_CORE_COLLECTION_H */
