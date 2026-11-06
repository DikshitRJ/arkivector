/*
 * arkiv/query_builder.h: safe construction of arkiv_query strings.
 *
 * Every user-influenced value goes through a literal constructor that validates and escapes it
 * (str('..') with '' escaping and a 128-byte cap, key(0x..), addr(0x..), typed numerics), so a
 * value can never close the literal and inject syntax. Output buffers are caller-provided
 * and fixed-size; exceeding the capacity is an AV_ERR ("invalid_argument"), never a truncation.
 * Thread-safe: no shared state.
 */
#ifndef AV_ARKIV_QUERY_BUILDER_H
#define AV_ARKIV_QUERY_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

#define AV_QB_STR_MAX   128   /* max bytes of a str literal value */
#define AV_QB_QUERY_MAX 16384 /* default maximum query length used by callers */
#define AV_QB_LIT_MAX   400   /* buffer size that fits any single literal */

/* Append-only query buffer. buf is NUL-terminated at all times. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} av_qb;

/* Bind a buffer (cap >= 1) and start with the empty query. */
void av_qb_init(av_qb *q, char *buf, size_t cap);

/* Single literals into out (cap >= AV_QB_LIT_MAX is always enough). */
av_status av_qb_lit_str(const char *s, size_t len, char *out, size_t cap, av_err *e);
av_status av_qb_lit_key(const uint8_t key[32], char *out, size_t cap, av_err *e);
av_status av_qb_lit_addr(const uint8_t addr[20], char *out, size_t cap, av_err *e);
av_status av_qb_lit_u64(uint64_t v, char *out, size_t cap, av_err *e);
av_status av_qb_lit_i32(int32_t v, char *out, size_t cap, av_err *e);
/* u256 from a decimal digit string (<= 78 digits); dec from [-]digits[.digits(<=18)]. */
av_status av_qb_lit_u256(const char *digits, char *out, size_t cap, av_err *e);
av_status av_qb_lit_dec(const char *dec, char *out, size_t cap, av_err *e);

/* Attribute-name check: [A-Za-z][A-Za-z0-9._-]{0,31}, no "--". */
bool av_qb_valid_name(const char *name);

/* Append "<name> <op> <literal>" (op one of = != < <= > >=). name must be valid or a $system
   field ($key, $owner, $creator, $expiresAt). literal comes from the av_qb_lit_* functions. */
av_status av_qb_cmp(av_qb *q, const char *name, const char *op, const char *literal, av_err *e);
/* Append " AND " / " OR " (nothing if the query is still empty). */
av_status av_qb_and(av_qb *q, av_err *e);
av_status av_qb_or(av_qb *q, av_err *e);
/* Append raw text produced by this builder (e.g. a parenthesised sub-query). */
av_status av_qb_raw(av_qb *q, const char *s, av_err *e);

/* "$key = key(0x..) OR $key = key(0x..) ..." for n >= 1 keys (a chunk; the caller halves n when
   the node answers -32004), or "$creator = addr(..) OR ..." for addresses. */
av_status av_qb_or_keys(char *out, size_t cap, const uint8_t (*keys)[32], size_t n, av_err *e);
av_status av_qb_or_creators(char *out, size_t cap, const uint8_t (*addrs)[20], size_t n, av_err *e);

/* Key-cursor page query: "(<base>) AND $key > key(<cursor>) ORDER BY $key ASC". base may be
   NULL/"" (match everything), cursor may be NULL (first page). base must itself have been
   built with this module (it is not parsed). */
av_status av_qb_paged(char *out, size_t cap, const char *base, const uint8_t cursor[32], av_err *e);

#endif /* AV_ARKIV_QUERY_BUILDER_H */
