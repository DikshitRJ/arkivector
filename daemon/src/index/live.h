/*
 * index/live.h: the live bitmap of a collection ("heap visibility", ARCHITECTURE 8.4)
 * with copy-on-publish snapshots.
 *
 * The published bitmap is immutable. The applier edits a private working copy (created
 * lazily on first use after a publish; roaring copy-on-write keeps the copy cheap) and
 * av_live_publish() swaps it in. Readers take a refcounted snapshot, so a bitmap they
 * hold never changes underneath them, even after later publishes.
 *
 * Thread safety:
 *  - av_live_working / av_live_publish / av_live_discard: applier thread only. av_live_publish
 *    must run while the owner holds its publish write lock, so it cannot race with
 *    av_live_acquire.
 *  - av_live_acquire: any thread, but only under the owner's publish read lock.
 *  - av_live_snap_bitmap / av_live_snap_release: any thread; release needs no lock.
 *  - av_live_current: valid only while the caller holds the publish read lock.
 */
#ifndef AV_INDEX_LIVE_H
#define AV_INDEX_LIVE_H

#include <roaring.h>

typedef struct av_live av_live;
typedef struct av_live_snap av_live_snap;

av_live *av_live_create(void);
void av_live_destroy(av_live *l);

const roaring_bitmap_t *av_live_current(const av_live *l);
av_live_snap *av_live_acquire(av_live *l);
const roaring_bitmap_t *av_live_snap_bitmap(const av_live_snap *s);
void av_live_snap_release(av_live_snap *s);

/* Applier: the editable copy (never NULL unless out of memory). */
roaring_bitmap_t *av_live_working(av_live *l);
/* Applier: make the working copy the published bitmap (no-op when untouched). */
void av_live_publish(av_live *l);
/* Applier: throw the working copy away. */
void av_live_discard(av_live *l);

#endif /* AV_INDEX_LIVE_H */
