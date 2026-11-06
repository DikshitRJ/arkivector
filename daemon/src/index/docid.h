/*
 * index/docid.h: DocId <-> key32 maps of one collection.
 *
 * A DocId is a monotonically increasing u32 per collection, assigned to every vector
 * version (ARCHITECTURE section 8.1). key -> DocId always names the CURRENT docid of a key;
 * the dense DocId -> key table keeps entries for superseded (tombstoned) docids too, so a
 * stale hit can still be mapped back to its key. After a restart only live docids are known:
 * av_docmap_key() returns NULL for the rest.
 *
 * Besides the key, each docid carries the entity's updated_at and expires_at block (used by
 * debug routes and the TTL pruner without an LMDB read).
 *
 * Thread safety: NONE inside. The collection mutates a docmap only while holding its
 * publish write lock and reads it only under the read lock.
 */
#ifndef AV_INDEX_DOCID_H
#define AV_INDEX_DOCID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

typedef uint32_t av_docid;
#define AV_DOCID_MAX UINT32_MAX

typedef struct av_docmap av_docmap;

av_docmap *av_docmap_create(void);
void av_docmap_destroy(av_docmap *m);

/* Point key at docid (replacing any previous mapping of key) and record docid -> key and meta. */
av_status av_docmap_assign(av_docmap *m, const uint8_t key[32], av_docid id, uint64_t updated_at,
                           uint64_t expires_at, av_err *e);
/* Forget key -> docid (the docid -> key entry stays). No-op when absent. */
void av_docmap_unmap_key(av_docmap *m, const uint8_t key[32]);
/* Update meta of an already assigned docid. */
void av_docmap_set_meta(av_docmap *m, av_docid id, uint64_t updated_at, uint64_t expires_at);

bool av_docmap_lookup(const av_docmap *m, const uint8_t key[32], av_docid *id);
/* NULL when the docid is unknown. The pointer is valid until the next mutation. */
const uint8_t *av_docmap_key(const av_docmap *m, av_docid id);
bool av_docmap_meta(const av_docmap *m, av_docid id, uint64_t *updated_at, uint64_t *expires_at);

/* Number of key -> docid mappings (current docids). */
size_t av_docmap_size(const av_docmap *m);
/* One past the highest docid recorded (a lower bound for the next docid). */
av_docid av_docmap_high(const av_docmap *m);

#endif /* AV_INDEX_DOCID_H */
