/*
 * store/checkpoint.h: rebuild helpers that load a collection's in-memory state from LMDB.
 * (The sync-side resume logic, cursor verification and reorg handling, is in sync/.)
 *
 * Thread safety: read-only; uses read transactions, so it may run beside the writer, but the
 * collection calls it only while it is being constructed.
 */
#ifndef AV_STORE_CHECKPOINT_H
#define AV_STORE_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"
#include "log.h"
#include "store/lmdb_store.h"

typedef struct {
    const uint8_t *key; /* 32 bytes */
    av_row row;
    av_attr *attrs; /* unpacked attribute list (arena memory) */
    size_t n_attrs;
    const uint8_t *vec; /* av.vec bytes, or NULL when the row has none */
    size_t vec_len;
} av_loaded_row;

/* Called once per stored row in key order. The arena behind attrs/vec is reset after the call. false stops. */
typedef bool (*av_checkpoint_row_cb)(const av_loaded_row *r, void *ctx);

/* Stream every entity row of coll. A corrupt row fails the load (code "internal"). */
av_status av_checkpoint_load_rows(av_store *s, const char *coll, av_checkpoint_row_cb cb, void *ctx,
                                  av_err *e);

/* Persisted bookkeeping: sync cursor (found=false before the first block) and the docid counter. */
typedef struct {
    bool has_cursor;
    av_cursor cursor;
    bool has_next_docid;
    uint32_t next_docid;
} av_checkpoint_meta;
av_status av_checkpoint_load_meta(av_store *s, const char *coll, av_checkpoint_meta *out, av_err *e);

#endif /* AV_STORE_CHECKPOINT_H */
