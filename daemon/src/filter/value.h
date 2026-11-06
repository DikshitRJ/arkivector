/*
 * filter/value.h: typed attribute values in canonical Arkiv encoding (D3).
 *
 * An av_value is a self-contained (no pointers, no ownership) typed value whose
 * bytes are exactly what an entity stores for that type
 * (RESEARCH/arkiv-docs/03-evm-mutations-and-contract-spec/attribute-serialization.md,
 * arkiv/entity.h):
 *   bool/i32/u64/u256/dec/addr/key : the 32-byte big-endian ABI word (len == 32)
 *   str                            : validated UTF-8, 0..128 bytes (len == byte count)
 * So equality with a stored attribute is memcmp, and av_value_cmp gives the
 * ordering of docs/api/filters.md section 3.
 *
 * Thread safety: all functions are pure and reentrant.
 */
#ifndef AV_FILTER_VALUE_H
#define AV_FILTER_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"

typedef struct {
    av_attr_type type;
    size_t len;                 /* 32 for word types, 0..AV_ATTR_STR_MAX for str */
    uint8_t b[AV_ATTR_STR_MAX]; /* canonical bytes; unused tail is zero */
} av_value;

/* Minimum buffer for av_value_format without truncation (str max 128, dec <= 80, hex 67, + NUL). */
#define AV_VALUE_FORMAT_MAX 160

/* "bool", "i32", ... ("bytes" included, "tombstone" -> "tombstone"). Never NULL. */
const char *av_type_name(av_attr_type t);

/* Case-sensitive type name lookup ("bytes" is accepted). false when unknown. */
bool av_type_from_name(const char *s, size_t n, av_attr_type *out);

/* True when the type can appear in a filter value (everything except bytes/tombstone). */
bool av_type_filterable(av_attr_type t);

/* Builders from C values (cannot fail). */
void av_value_bool(av_value *v, bool x);
void av_value_i32(av_value *v, int32_t x);
void av_value_u64(av_value *v, uint64_t x);
void av_value_addr(av_value *v, const uint8_t addr[20]);
void av_value_key(av_value *v, const uint8_t key[32]);
/* u256 / dec from a raw 32-byte big-endian word (dec: two's complement, scale 1e18). */
void av_value_u256_word(av_value *v, const uint8_t word[32]);
void av_value_dec_word(av_value *v, const uint8_t word[32]);
/* str: false when n > 128 or the bytes are not valid UTF-8. */
bool av_value_str(av_value *v, const uint8_t *s, size_t n);

/*
 * Wrap the stored canonical bytes of an attribute (what the evaluator holds).
 * Returns false when the length or the padding is not canonical for the type
 * (e.g. an i32 word that is not a sign-extended 32-bit integer) or the type is not filterable.
 */
bool av_value_from_stored(av_attr_type t, const uint8_t *p, size_t n, av_value *out);

/*
 * Parse the text form of a value of type t (the content of a where-literal
 * constructor or a JSON string value):
 *   bool  : "true" | "false" (case-insensitive)
 *   i32   : -?digits within int32;   u64: digits within uint64
 *   u256  : digits or 0x-hex (1..64 digits) within 2^256-1
 *   dec   : -?digits[.digits], at most 18 fraction digits, within signed 256-bit
 *   str   : the raw text itself (UTF-8, <= 128 bytes)
 *   addr  : 0x + 40 hex; mixed case must be a valid EIP-55 checksum (all-lower / all-upper are accepted)
 *   key   : 0x + 64 hex
 * No whitespace or sign '+' is accepted. On failure returns false and, when why != NULL,
 * sets *why to a static message. bytes is never accepted.
 */
bool av_value_parse(av_attr_type t, const char *s, size_t n, av_value *out, const char **why);

/*
 * Format like snprintf: writes at most cap bytes including the NUL and returns the
 * length the full text needs (excluding NUL). Forms: bool true/false; i32/u64/u256
 * decimal; dec decimal with trailing fraction zeros trimmed ("4", "4.5", "-0.25");
 * str raw bytes; addr/key lowercase 0x hex. cap == 0 allowed (buf may be NULL).
 */
size_t av_value_format(const av_value *v, char *buf, size_t cap);

/*
 * Three-way compare (<0, 0, >0). Same type: i32 and dec as signed 256-bit, u64/u256 as
 * unsigned, bool false<true, addr/key unsigned big-endian bytes, str bytewise UTF-8
 * (memcmp then length). Different types: ordered by type code (never meaningful for a
 * filter; the evaluator only compares equal types).
 */
int av_value_cmp(const av_value *a, const av_value *b);
bool av_value_equal(const av_value *a, const av_value *b);

/* Strict UTF-8 validation (no overlongs, surrogates or code points above U+10FFFF). */
bool av_utf8_valid(const uint8_t *s, size_t n);

#endif /* AV_FILTER_VALUE_H */
