/*
 * filter/parser.h: `where` string -> filter AST (docs/api/filters.md section 4).
 *
 * Recursive descent over the grammar
 *   or_expr := and_expr (OR and_expr)* ; and_expr := unary (AND unary)*
 *   unary := NOT unary | ( or_expr ) | predicate
 * AND/OR chains become one n-ary node; parenthesised groups are kept as written (so
 * `a AND (b AND c)` has a nested AND, like its JSON twin). A one-element group collapses
 * to its element.
 */
#ifndef AV_FILTER_PARSER_H
#define AV_FILTER_PARSER_H

#include <stddef.h>

#include "core/arena.h"
#include "core/coldef.h"
#include "filter/ast.h"
#include "log.h"

/* Maximum recursion (NOT and parenthesis nesting) before the parser gives up. */
#define AV_FILTER_MAX_NEST 32

/*
 * Parse the `where` text s[0..n) (not necessarily NUL-terminated; n <= AV_FILTER_MAX_WHERE_BYTES).
 *
 * sch: the collection definition (docs name it av_attr_schema; the type is av_coldef) used
 * to reject attributes the schema declares as bytes. May be NULL. Literals in `where` are
 * always explicitly typed, so the schema never changes literal types.
 *
 * Ownership: *out and everything below it live in arena a. Untrusted input.
 * Errors: AV_ERR with code "invalid_filter", message, e->position = byte offset of the
 * offending token (or "internal" on allocation failure). *out is NULL on error.
 */
av_status av_filter_from_where(const char *s, size_t n, const av_coldef *sch, av_arena *a, av_filter **out,
                               av_err *e);

#endif /* AV_FILTER_PARSER_H */
