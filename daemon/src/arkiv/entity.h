/*
 * arkiv/entity.h: Arkiv entities as the daemon mirrors them (cross-lane contract, D2).
 *
 * Attribute values are kept in their canonical on-chain encoding
 * (RESEARCH/arkiv-docs/03-evm-mutations-and-contract-spec/attribute-serialization.md):
 *   bool/i32/u64/u256/dec/addr/key : the 32-byte big-endian ABI word
 *   str                            : UTF-8 bytes (<= 128), no NUL terminator
 *   bytes                          : raw bytes (<= 128 KiB)
 * So equality is memcmp, and ordering per type is defined in filter/value.h (D3).
 *
 * Ownership: av_entity and everything it points to live in the arena passed to the
 * parser (arkiv/entity.c) or copier (av_entity_copy).
 */
#ifndef AV_ARKIV_ENTITY_H
#define AV_ARKIV_ENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "log.h"

/* Arkiv type codes (typeId & 0x7F). */
typedef enum {
    AV_AT_TOMBSTONE = 0,
    AV_AT_BOOL = 1,
    AV_AT_I32 = 2,
    AV_AT_U64 = 3,
    AV_AT_U256 = 4,
    AV_AT_DEC = 5,
    AV_AT_BYTES = 7,
    AV_AT_STR = 8,
    AV_AT_ADDR = 9,
    AV_AT_KEY = 10
} av_attr_type;

#define AV_ATTR_NAME_MAX 32
#define AV_ATTR_STR_MAX 128
#define AV_ATTR_BYTES_MAX (128 * 1024)

/* Reserved attribute names (docs/api/collections.md §4). */
#define AV_ATTR_SCHEMA "arkiv.schema" /* str, indexed: collection schema tag */
#define AV_ATTR_VEC "av.vec"          /* bytes: VectorBytes v1 */
#define AV_ATTR_ID "av.id"            /* str, indexed: external id */
#define AV_ATTR_DOC "av.doc"          /* bytes: UTF-8 document */

typedef struct {
    char name[AV_ATTR_NAME_MAX + 1]; /* NUL-terminated */
    uint8_t type;                    /* av_attr_type */
    bool indexed;
    const uint8_t *val; /* canonical bytes (see top of file) */
    size_t len;
} av_attr;

typedef struct {
    uint8_t key[32];
    uint8_t owner[20];
    uint8_t creator[20];
    uint64_t created_at; /* block numbers */
    uint64_t updated_at;
    uint64_t expires_at;
    av_attr *attrs; /* includes arkiv.schema, av.vec, av.id, av.doc */
    size_t n_attrs;
} av_entity;

/* Find an attribute by name (NULL if absent). */
const av_attr *av_entity_attr(const av_entity *e, const char *name);

/* Deep copy into arena a. */
av_status av_entity_copy(const av_entity *src, av_arena *a, av_entity *dst, av_err *err);

/* 32-byte key <-> "0x" + 64 lowercase hex (buffer >= 67 bytes). */
void av_key_to_hex(const uint8_t key[32], char out[67]);
bool av_key_from_hex(const char *s, uint8_t key[32]);
void av_addr_to_hex(const uint8_t addr[20], char out[43]);
bool av_addr_from_hex(const char *s, uint8_t addr[20]);

#endif /* AV_ARKIV_ENTITY_H */
