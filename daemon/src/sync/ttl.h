/*
 * sync/ttl.h: passive TTL reclamation scheduler (ARCHITECTURE §10.2).
 *
 * A binary min-heap of (expires_at, docid, generation) with lazy deletion: every
 * set/remove bumps the docid's generation, so older heap entries become stale and are
 * skipped when popped. Correctness of reads never depends on this scheduler (liveness
 * is checked per query as expires_at > as_of_block); it only reclaims storage.
 *
 * Thread safety: applier thread only (no internal locking).
 */
#ifndef AV_SYNC_TTL_H
#define AV_SYNC_TTL_H

#include <stddef.h>
#include <stdint.h>

#include "log.h"

typedef struct av_ttl av_ttl;

av_ttl *av_ttl_create(void);
void av_ttl_destroy(av_ttl *t);

/* Insert or extend: the docid expires at block expires_at (0 = never). */
av_status av_ttl_set(av_ttl *t, uint32_t docid, uint64_t expires_at);

/* The docid is gone (deleted/tombstoned): drop it from the schedule. */
void av_ttl_remove(av_ttl *t, uint32_t docid);

/*
 * Pop up to max docids whose expiry is <= block (current, non-stale entries only),
 * in expiry order, into out[]. Returns the number written. Popped docids are removed
 * from the schedule.
 */
size_t av_ttl_pop_expired(av_ttl *t, uint64_t block, uint32_t *out, size_t max);

/* Current expiry of a docid (0 if unscheduled). */
uint64_t av_ttl_expiry(const av_ttl *t, uint32_t docid);

/* Heap entries including stale ones (for metrics/tests). */
size_t av_ttl_heap_size(const av_ttl *t);

/* Rebuild the heap discarding stale entries (call when heap_size >> live count). */
av_status av_ttl_compact(av_ttl *t);

#endif /* AV_SYNC_TTL_H */
