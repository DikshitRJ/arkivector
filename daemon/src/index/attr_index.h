/*
 * index/attr_index.h: attribute postings of one collection (ARCHITECTURE section 8.3).
 *
 * For every (name, type) a sorted vector of distinct canonical values, each with a roaring
 * bitmap of the docids holding it; per (name, type) a "typed" bitmap; per name an "exists"
 * bitmap. Values use the canonical encodings of arkiv/entity.h (32-byte ABI words for
 * bool/i32/u64/u256/dec/addr/key, raw bytes for str/bytes). Ordering per type:
 *   i32, dec : signed two's-complement 256-bit word
 *   others   : unsigned big-endian words / lexicographic bytes (memcmp, then length)
 * bytes values longer than AV_ATTR_STR_MAX are not value-indexed (exists/typed only).
 *
 * System fields are plain attributes named "$owner" and "$creator" (type addr, the 20-byte
 * address left-padded to a 32-byte word) and "$expiresAt" (type u64, 32-byte big-endian
 * word); core/collection.c inserts them next to the entity's own attributes.
 *
 * Update protocol: av_attr_index_insert appends new distinct values to a per-field staging
 * list, which av_attr_index_remove and av_attr_index_flush merge into the sorted vector in
 * one O(n + m log m) pass. Lookups (exists/typed/eq/scan) only see flushed data, so the
 * writer calls av_attr_index_flush() at the end of each batch of edits.
 *
 * Thread safety: edits and flush by one writer thread, which the collection runs under the
 * publish write lock. Lookups and scans are read-only and safe from any number of threads
 * under the publish read lock. Returned bitmaps are valid only until the next flush.
 */
#ifndef AV_INDEX_ATTR_INDEX_H
#define AV_INDEX_ATTR_INDEX_H

#include <roaring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "index/docid.h"
#include "log.h"

typedef struct av_attr_index av_attr_index;

av_attr_index *av_attr_index_create(void);
void av_attr_index_destroy(av_attr_index *ix);

av_status av_attr_index_insert(av_attr_index *ix, av_docid id, const char *name, uint8_t type,
                               const uint8_t *val, size_t len, av_err *e);
av_status av_attr_index_remove(av_attr_index *ix, av_docid id, const char *name, uint8_t type,
                               const uint8_t *val, size_t len, av_err *e);
av_status av_attr_index_flush(av_attr_index *ix, av_err *e);

/* NULL when the name / (name,type) / value is unknown. */
const roaring_bitmap_t *av_attr_index_exists(const av_attr_index *ix, const char *name);
const roaring_bitmap_t *av_attr_index_typed(const av_attr_index *ix, const char *name, uint8_t type);
const roaring_bitmap_t *av_attr_index_eq(const av_attr_index *ix, const char *name, uint8_t type,
                                         const uint8_t *val, size_t len);

/* Ordered iteration over distinct values in [lo, hi] (NULL bound = unbounded). Return false to stop. */
typedef bool (*av_attr_scan_cb)(const uint8_t *val, size_t len, const roaring_bitmap_t *postings, void *ctx);
void av_attr_index_scan(const av_attr_index *ix, const char *name, uint8_t type, const uint8_t *lo,
                        size_t lo_len, bool lo_incl, const uint8_t *hi, size_t hi_len, bool hi_incl,
                        av_attr_scan_cb cb, void *ctx);

/* Number of distinct indexed values of (name, type). */
size_t av_attr_index_value_count(const av_attr_index *ix, const char *name, uint8_t type);

/* Ordering used by the index (negative, zero, positive). */
int av_attr_value_cmp(uint8_t type, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

#endif /* AV_INDEX_ATTR_INDEX_H */
