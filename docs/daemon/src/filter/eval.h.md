# daemon/src/filter/eval.h

## Purpose

Declares the AST-to-Roaring bitmap filter evaluation interface for Arkivector (Phase D3). It evaluates abstract syntax tree filter expressions against the collection's attribute postings index (`av_attr_index`) and document map (`av_docmap`), pruning expired entities and restricting candidate sets to live documents.

## Key Types and Functions

| Symbol | Role |
| :--- | :--- |
| `av_filter_eval` | Evaluates a filter AST node `f` against `ix` and `docmap`, intersects with `live` docids, prunes expired entities as of `as_of_block`, and returns a new Roaring bitmap of matching docids. |

### Function Signature

```c
roaring_bitmap_t *av_filter_eval(const av_filter *f, const av_attr_index *ix, const av_docmap *docmap,
                                 const roaring_bitmap_t *live, uint64_t as_of_block, av_err *e);
```

## Memory Ownership

- **Return Value**: Returns a heap-allocated `roaring_bitmap_t *`. The caller owns the returned bitmap and must release it with `roaring_bitmap_free()`.
- **Inputs**: The filter AST (`f`), attribute index (`ix`), document map (`docmap`), and live bitmap (`live`) are borrowed for reading and remain owned by the caller/collection.
- **Error Object**: On failure or memory exhaustion, sets `e` (if non-NULL) and returns `NULL`.

## Invariants

- If `f == NULL` or `f->kind == AV_F_TRUE`, matches all live, unexpired entities in the collection.
- If `live == NULL`, returns an empty bitmap (cardinality 0).
- Every returned docid is strictly live (`roaring_bitmap_contains(live, id) == true`).
- Every returned docid is strictly unexpired at block height `as_of_block` (`expires_at_block > as_of_block`).

## Error Handling

- Out-of-memory during Roaring bitmap operations sets `code = "internal"` with message `"out of memory"` in `e` and returns `NULL`.
- Invalid filter constructs (e.g. malformed `NOT` node or non-string operand to `STARTSWITH`) set `code = "invalid_filter"` in `e` and return `NULL`.

## Thread Safety

- `av_filter_eval` is reentrant and thread-safe for concurrent readers.
- `ix`, `docmap`, and `live` must not be concurrently modified without synchronization (guaranteed by the collection's staged publish read-write lock).
- Returned Roaring bitmap is thread-local to the caller.
