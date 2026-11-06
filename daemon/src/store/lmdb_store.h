/*
 * store/lmdb_store.h: LMDB-backed persistent state (entity rows, id maps, cursors, reorg
 * ring, quarantine, collection definitions). Layout: store/schema.h.
 *
 * Thread safety: one av_store per data dir. Exactly one thread (the applier) may hold an
 * av_txn at a time (a second av_store_begin blocks until the first ends). All av_store_get_*,
 * av_store_scan_* and av_store_list_* functions open their own read transaction and may run
 * on any thread concurrently with the writer (MDB_NOTLS). Read transactions see the last
 * committed state. The txn-scoped getters (av_store_txn_*) see the transaction's own writes
 * and must be called by the thread that owns the txn.
 *
 * Collection DB handles are cached under a mutex. Handles created in a txn become visible
 * to readers when the txn commits (and are dropped if it aborts).
 *
 * Ownership: arena arguments receive copies; pointers handed to scan callbacks point into
 * LMDB memory and are valid only during the callback.
 */
#ifndef AV_STORE_LMDB_STORE_H
#define AV_STORE_LMDB_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "log.h"

#define AV_STORE_DEFAULT_MAP_SIZE ((size_t) 64 << 30)

typedef struct av_store av_store;
/* dir is created if missing. map_size 0 selects AV_STORE_DEFAULT_MAP_SIZE (sparse). */
av_status av_store_open(const char *dir, size_t map_size, av_store **out, av_err *e);
void av_store_close(av_store *s);

typedef struct av_txn av_txn; /* write txn (applier only) */
av_status av_store_begin(av_store *s, av_txn **t, av_err *e);
av_status av_store_commit(av_txn *t, av_err *e); /* frees t */
void av_store_abort(av_txn *t);                  /* frees t */

/* collections */
av_status av_store_put_collection(av_txn *t, const char *name, const char *json, size_t len, av_err *e);
av_status av_store_del_collection(av_txn *t, const char *name, av_err *e); /* drops all its DBs */
typedef bool (*av_store_coll_cb)(const char *name, const char *json, size_t len, void *ctx);
av_status av_store_list_collections(av_store *s, av_store_coll_cb cb, void *ctx, av_err *e);

/* entity rows: key32 -> packed row */
typedef struct {
    uint32_t docid;
    uint8_t owner[20];
    uint8_t creator[20];
    uint64_t created_at, updated_at, expires_at;
    uint8_t vec_hash[32];
    const uint8_t *attrs_blob;
    size_t attrs_len;
} av_row;

av_status av_store_put_entity(av_txn *t, const char *coll, const uint8_t key[32], const av_row *row,
                              av_err *e);
av_status av_store_del_entity(av_txn *t, const char *coll, const uint8_t key[32], av_err *e);
/* out->attrs_blob is copied into arena a. */
av_status av_store_get_entity(av_store *s, const char *coll, const uint8_t key[32], av_arena *a,
                              av_row *out, bool *found, av_err *e);
av_status av_store_txn_get_entity(av_txn *t, const char *coll, const uint8_t key[32], av_arena *a,
                                  av_row *out, bool *found, av_err *e);
typedef bool (*av_store_row_cb)(const uint8_t key[32], const av_row *row, void *ctx); /* false stops */
av_status av_store_scan_entities(av_store *s, const char *coll, av_store_row_cb cb, void *ctx, av_err *e);
av_status av_store_count_entities(av_store *s, const char *coll, size_t *n, av_err *e);

/* external id -> key */
av_status av_store_put_id(av_txn *t, const char *coll, const char *id, size_t idlen, const uint8_t key[32],
                          av_err *e);
av_status av_store_del_id(av_txn *t, const char *coll, const char *id, size_t idlen, av_err *e);
av_status av_store_get_id(av_store *s, const char *coll, const char *id, size_t idlen, uint8_t key[32],
                          bool *found, av_err *e);
av_status av_store_txn_get_id(av_txn *t, const char *coll, const char *id, size_t idlen, uint8_t key[32],
                              bool *found, av_err *e);

/* sync cursor and reorg ring */
typedef struct {
    uint64_t block;
    uint8_t hash[32];
} av_cursor;

typedef struct {
    uint64_t block;
    uint8_t hash[32];
    uint8_t (*keys)[32];
    size_t n_keys;
} av_ring_entry;

av_status av_store_put_cursor(av_txn *t, const char *coll, const av_cursor *c, av_err *e);
av_status av_store_get_cursor(av_store *s, const char *coll, av_cursor *c, bool *found, av_err *e);
av_status av_store_ring_put(av_txn *t, const char *coll, uint64_t block, const uint8_t hash[32],
                            const uint8_t (*keys)[32], size_t n_keys, av_err *e);
/* *keys is arena memory (n_keys entries). found=false when the block is not in the ring. */
av_status av_store_ring_get(av_store *s, const char *coll, uint64_t block, uint8_t hash[32], av_arena *a,
                            uint8_t (**keys)[32], size_t *n_keys, bool *found, av_err *e);
/* Lists ring entries newest-first. entries and their keys are allocated in arena a. */
av_status av_store_ring_list(av_store *s, const char *coll, av_arena *a, av_ring_entry **out, size_t *n_out,
                            av_err *e);
/* Keep only the newest keep_last blocks. */
av_status av_store_ring_trim(av_txn *t, const char *coll, size_t keep_last, av_err *e);
/* Delete all ring entries with block > from_block (after a reorg reset). */
av_status av_store_ring_drop_above(av_txn *t, const char *coll, uint64_t from_block, av_err *e);

/* quarantine: entities that matched the collection but carry an invalid av.vec */
av_status av_store_quarantine(av_txn *t, const char *coll, const uint8_t key[32], const char *reason,
                              av_err *e);
av_status av_store_quarantine_put(av_txn *t, const char *coll, const uint8_t key[32], uint64_t updated_at,
                                  const char *reason, av_err *e);
av_status av_store_unquarantine(av_txn *t, const char *coll, const uint8_t key[32], av_err *e);
av_status av_store_txn_get_quarantine(av_txn *t, const char *coll, const uint8_t key[32],
                                      uint64_t *updated_at, bool *found, av_err *e);
av_status av_store_count_quarantine(av_store *s, const char *coll, size_t *n, av_err *e);

/* docid counter (meta) */
av_status av_store_put_next_docid(av_txn *t, const char *coll, uint32_t next, av_err *e);
av_status av_store_get_next_docid(av_store *s, const char *coll, uint32_t *next, bool *found, av_err *e);

#endif /* AV_STORE_LMDB_STORE_H */
