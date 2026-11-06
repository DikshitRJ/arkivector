/*
 * filter/json_filter.h: JSON AST -> filter AST (docs/api/filters.md section 2).
 *
 * Each node is an object with exactly one operator key: and, or, not, eq, ne, lt, lte, gt,
 * gte, between, startswith, exists, typeof. Values are TypedValues
 * ({"type": "dec", "value": "4.0"}, exactly those two keys) or shorthand scalars typed by
 * (1) the collection schema entry of the field, else (2) the system-field type, else
 * (3) the JSON type (string -> str, boolean -> bool, integer -> i32). Value encodings
 * accepted per type: bool: JSON boolean; i32/u64/u256/dec: JSON integer or decimal string
 * (u256 also 0x-hex string; JSON reals are rejected, send dec as a string); str: JSON string;
 * addr/key: 0x-hex string. Unknown keys anywhere, wrong arity or wrong JSON types are errors.
 */
#ifndef AV_FILTER_JSON_FILTER_H
#define AV_FILTER_JSON_FILTER_H

#include "core/arena.h"
#include "core/coldef.h"
#include "filter/ast.h"
#include "log.h"
#include "yyjson.h"

/*
 * Convert the JSON filter v (borrowed, not retained) into an AST in arena a.
 * sch: collection definition used for shorthand typing and the bytes check; may be NULL.
 * Errors: AV_ERR with code "invalid_filter", a message, e->field = JSON pointer of the
 * offending node ("/and/1/gte/score"), e->position = -1; "internal" on allocation failure.
 * *out is NULL on error. Untrusted input.
 */
av_status av_filter_from_json(yyjson_val *v, const av_coldef *sch, av_arena *a, av_filter **out, av_err *e);

#endif /* AV_FILTER_JSON_FILTER_H */
