# daemon/src/filter/eval.c

## Purpose

Implements the filter AST evaluator declared in `eval.h`. It resolves predicates against sorted attribute postings and document metadata into compressed Roaring bitmaps, combining boolean logic with set operations (`AND`, `OR`, `ANDNOT`), range scans, prefix scans, and TTL expiration pruning.

## Key Types and Functions

| Function / Type | Role |
| :--- | :--- |
| `scan_ctx` | Context structure (`bm`, `prefix`, `prefix_len`) passed to index scan callbacks. |
| `scan_or_cb` | Index range scan callback that bitwise-ORs visited postings into an accumulator bitmap. |
| `scan_startswith_cb` | Index prefix scan callback that verifies the string prefix and bitwise-ORs matching postings until out-of-range. |
| `key_scan_ctx` | Context structure (`docmap`, `target_key`, `high_key`, `op`, `is_between`, `out`) for iterating key comparisons over docids. |
| `key_cmp_cb` | Callback iterating live docids to test binary comparisons (`EQ`, `NE`, `LT`, `LE`, `GT`, `GE`) or `BETWEEN` on 32-byte `$key`. |
| `eval_node` | Recursive internal evaluator matching AST node types (`AV_F_AND`, `AV_F_OR`, `AV_F_NOT`, `AV_F_EXISTS`, `AV_F_TYPEOF`, `AV_F_CMP`, `AV_F_BETWEEN`, `AV_F_STARTSWITH`) to postings index or docmap operations. |
| `av_filter_eval` | Public entrypoint. Invokes `eval_node`, performs in-place intersection with `live`, extracts docids to a temporary array, queries `av_docmap_meta`, and removes expired entries (`exp <= as_of_block`). |

## Memory Ownership

- **Bitmaps**: Intermediate bitmaps created during recursive evaluation of children (`eval_node`) are freed immediately via `roaring_bitmap_free()` once aggregated into parent results.
- **Expiration Array**: In `av_filter_eval`, an array `docids` of size `cardinality * sizeof(uint32_t)` is allocated with `malloc` to inspect entity metadata, and freed before exit.
- **Return Value**: Caller owns the returned `roaring_bitmap_t *` and must call `roaring_bitmap_free()`.

## Invariants

- **Short-Circuit AND**: If an intermediate result bitmap becomes empty in `AV_F_AND`, remaining child expressions are skipped.
- **Typed Matching**: Equality (`AV_OP_EQ`) and inequality (`AV_OP_NE`) attribute comparisons are checked against both attribute name and wire type (`attr_type`). `NE` is computed as `typed ANDNOT eq`.
- **System Fields**: System attributes (`$key`, `$owner`, `$creator`, `$expires_at`) are resolved through `docmap` queries and type validation rather than attribute index postings.
- **TTL Expiration Pruning**: Entities where `expires_at_block <= as_of_block` are unconditionally removed from the candidate bitmap.

## Error Handling

- Out-of-memory errors allocate nothing and set `av_err` with code `"internal"` and message `"out of memory"`.
- Semantic validation errors (e.g. `NOT` operator with `n_kids != 1` or `STARTSWITH` on non-string type) set code `"invalid_filter"`.
- On error, all partially constructed bitmaps are freed before returning `NULL`.

## Thread Safety

- The evaluator is stateless and operates exclusively on read-only pointers (`av_attr_index`, `av_docmap`, `live`).
- Multiple query worker threads can execute `av_filter_eval` concurrently on the same collection state.
