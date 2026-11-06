/*
 * filter/ast.h: the filter AST shared by the `where` parser, the JSON parser and the
 * bitmap evaluator (D3). docs/api/filters.md is normative.
 *
 * A filter is an immutable tree of av_filter nodes allocated in the arena passed to the
 * parser; it is valid until that arena is reset or destroyed, and may be read by many
 * threads at once (no mutation after construction).
 *
 * Semantics (docs/api/filters.md section 3) the evaluator must implement per node:
 *   CMP        field exists with exactly value.type AND av_value_cmp(stored, value) satisfies op.
 *              NE therefore does NOT match entities lacking the field or storing another type.
 *   BETWEEN    same type as value, value <= stored <= high (inclusive; low > high matches nothing)
 *   STARTSWITH stored is str and its bytes start with value.b[0..value.len) (empty prefix: any str)
 *   EXISTS     field present (any type); TYPEOF: field present with that type
 *   NOT        logical complement over the candidate set (so NOT(eq) DOES match absent fields)
 *   AND/OR     n-ary, n_kids >= 2 ; TRUE matches everything
 * System fields are always present: $key (key), $owner/$creator (addr), $expiresAt (u64).
 *
 * Invariants guaranteed by every constructor in this module (parsers and builders):
 *   - depth <= AV_FILTER_MAX_DEPTH, leaves <= AV_FILTER_MAX_LEAVES
 *   - CMP/BETWEEN/STARTSWITH values have a filterable type; for system fields the value type
 *     equals the system-field type; STARTSWITH values are str on user attributes only
 *   - attribute names are valid, non-reserved Arkiv identifiers (<= 32 bytes)
 *   - the schema (when given) does not declare the attribute as bytes
 */
#ifndef AV_FILTER_AST_H
#define AV_FILTER_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/coldef.h"
#include "filter/value.h"
#include "log.h"

#define AV_FILTER_MAX_DEPTH      16   /* docs/api/filters.md section 2 */
#define AV_FILTER_MAX_LEAVES     256  /* docs/api/filters.md section 2 */
#define AV_FILTER_MAX_WHERE_BYTES 16384 /* size of a `where` string (mock/Arkiv query limit) */

typedef enum {
    AV_F_TRUE = 0, /* match everything (only built through av_filter_true) */
    AV_F_AND,
    AV_F_OR,
    AV_F_NOT,
    AV_F_CMP,
    AV_F_BETWEEN,
    AV_F_STARTSWITH,
    AV_F_EXISTS,
    AV_F_TYPEOF
} av_filter_kind;

typedef enum { AV_OP_EQ = 0, AV_OP_NE, AV_OP_LT, AV_OP_LE, AV_OP_GT, AV_OP_GE } av_cmp_op;

typedef enum {
    AV_FIELD_ATTR = 0, /* user attribute, `name` is the attribute name */
    AV_FIELD_KEY,      /* $key        (key)  */
    AV_FIELD_OWNER,    /* $owner      (addr) */
    AV_FIELD_CREATOR,  /* $creator    (addr) */
    AV_FIELD_EXPIRES   /* $expiresAt  (u64)  */
} av_field_kind;

typedef struct {
    av_field_kind kind;
    char name[AV_ATTR_NAME_MAX + 1]; /* attribute name, or the "$..." spelling for system fields */
} av_field;

typedef struct av_filter av_filter;
struct av_filter {
    av_filter_kind kind;
    int pos;      /* byte offset in the `where` text of the node start; -1 for JSON/API-built nodes */
    int depth;    /* 1 for a leaf, 1 + max(child depth) for AND/OR/NOT */
    av_filter **kids; /* AND/OR: n_kids >= 2 ; NOT: exactly 1 ; else NULL */
    size_t n_kids;
    av_field field;     /* leaves except TRUE */
    av_cmp_op op;       /* CMP */
    av_value value;     /* CMP operand, BETWEEN low, STARTSWITH prefix */
    av_value high;      /* BETWEEN high */
    av_attr_type type;  /* TYPEOF */
};

/*
 * Parse errors: av_filter_from_where / av_filter_from_json (parser.h, json_filter.h) return
 * AV_ERR with code "invalid_filter", a message, e->position (where: byte offset; JSON: -1)
 * and e->field (JSON: JSON pointer of the offending node; where: empty). Allocation failure
 * gives code "internal".
 */

/* True when `name` is a valid user attribute identifier (not reserved); *why gets a reason if not. */
bool av_filter_valid_attr_name(const char *name, size_t n, const char **why);

/* Debug / explain text of the tree, in `where` syntax with explicit parentheses around
 * every AND/OR group and explicit typed constructors (re-parsable by av_filter_from_where
 * except for AV_F_TRUE, which prints "true"). Allocated in a; NULL on allocation failure. */
char *av_filter_to_string(const av_filter *f, av_arena *a);

/* Structural equality (kind, field, operator, values, children; `pos` is ignored). */
bool av_filter_equal(const av_filter *a, const av_filter *b);

/* The match-everything filter. NULL on allocation failure. */
av_filter *av_filter_true(av_arena *a);

/* ---- construction helpers shared by the two parsers (and usable for programmatic filters) ---- */

typedef struct {
    av_arena *arena;
    const av_coldef *schema; /* may be NULL */
    size_t leaves;
} av_fbuild;

void av_fb_init(av_fbuild *fb, av_arena *a, const av_coldef *schema);

/* Resolve a field spelling ("$key", "category", ...) with validation. n is the byte length. */
bool av_fb_field(av_fbuild *fb, const char *name, size_t n, int pos, av_field *out, av_err *e);

/* Type of a field for typing shorthand JSON values: schema entry, else system type, else
 * AV_AT_TOMBSTONE (meaning "unknown"). */
av_attr_type av_fb_field_type(const av_fbuild *fb, const av_field *f);

/* Each returns NULL (and fills e, code "invalid_filter" or "internal") on failure. */
av_filter *av_fb_cmp(av_fbuild *fb, int pos, const av_field *f, av_cmp_op op, const av_value *v, av_err *e);
av_filter *av_fb_between(av_fbuild *fb, int pos, const av_field *f, const av_value *lo, const av_value *hi,
                         av_err *e);
av_filter *av_fb_startswith(av_fbuild *fb, int pos, const av_field *f, const av_value *prefix, av_err *e);
av_filter *av_fb_exists(av_fbuild *fb, int pos, const av_field *f, av_err *e);
av_filter *av_fb_typeof(av_fbuild *fb, int pos, const av_field *f, av_attr_type t, av_err *e);
av_filter *av_fb_not(av_fbuild *fb, int pos, av_filter *kid, av_err *e);
/* kind AND or OR; kids (n >= 1) is copied. A single kid is returned as is. */
av_filter *av_fb_group(av_fbuild *fb, av_filter_kind kind, int pos, av_filter *const *kids, size_t n, av_err *e);

/* Set code "invalid_filter" with printf-style message and position (pos < 0: unset). */
void av_filter_err(av_err *e, int pos, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif /* AV_FILTER_AST_H */
