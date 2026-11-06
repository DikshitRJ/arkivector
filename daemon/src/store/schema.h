/*
 * store/schema.h: on-disk layout of the LMDB store (cross-lane contract, D2).
 *
 * Thread safety: pure functions, no shared state. All integers are little-endian,
 * except block-number keys in ring:<coll>, which are big-endian so LMDB's byte order
 * sorts them numerically.
 *
 * LMDB environment (one per data dir, MDB_NOTLS, single writer):
 *   meta               "schema_version"        -> u32 AV_STORE_SCHEMA_VERSION
 *                      "next_docid:<coll>"     -> u32 next docid to allocate
 *   collections        <name>                  -> definition JSON (core/coldef.c)
 *   entities:<coll>    key32                   -> packed row (below)
 *   ids:<coll>         av.id bytes             -> key32
 *   cursor:<coll>      "c"                     -> u64 block, hash32
 *   ring:<coll>        u64 block (BE)          -> hash32, u32 n, n * key32
 *   quarantine:<coll>  key32                   -> u64 updated_at, reason bytes (UTF-8)
 *
 * Packed row, version 1 (AV_ROW_VERSION):
 *   off  size  field
 *     0     1  version (1)
 *     1     1  reserved (0)
 *     2     4  docid
 *     6    20  owner
 *    26    20  creator
 *    46     8  created_at
 *    54     8  updated_at
 *    62     8  expires_at
 *    70    32  vec_hash (SHA3-256 of the av.vec bytes)
 *   102     4  attrs_len
 *   106     n  attrs blob
 *
 * Attrs blob, version 1 (the full attribute list, av.vec and av.doc bytes included,
 * so a restart can rebuild the flat segment from the rows alone):
 *   u16 count, then per attribute:
 *     u8 type, u8 indexed, u8 name_len (1..AV_ATTR_NAME_MAX), name bytes,
 *     u32 val_len, val bytes
 *
 * Unpacking untrusted bytes checks every length against the buffer end.
 */
#ifndef AV_STORE_SCHEMA_H
#define AV_STORE_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"
#include "core/arena.h"
#include "log.h"

#define AV_STORE_SCHEMA_VERSION 1u
#define AV_ROW_VERSION 1u
#define AV_ROW_HEADER_SIZE 106u

/* Serialize attributes. *out is arena memory. */
av_status av_attrs_pack(const av_attr *attrs, size_t n, av_arena *a, uint8_t **out, size_t *len, av_err *e);

/*
 * Parse a blob into arena memory. The attribute values point into the arena copy, not into
 * buf, so buf (for example LMDB memory) need not outlive the result.
 */
av_status av_attrs_unpack(const uint8_t *buf, size_t len, av_arena *a, av_attr **out, size_t *n, av_err *e);

#endif /* AV_STORE_SCHEMA_H */
