# daemon/src/codec/text.c

## Purpose

Implements the text conversion API in `text.h`.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `in_fn` / `out_fn` | Map an `av_vtype` to the pgvector function: `vector_in`, `halfvec_in`, `sparsevec_in`, `bit_in`, and their `_out` counterparts. |
| `within_limits` | Apply the VectorBytes dimension and `nnz` limits to a parsed struct. A bit value of zero length is allowed. |
| `av_text_to_pgvecn` | Check the length and NUL byte. Create a memory context. Copy the literal with `pnstrdup`. Call the `_in` function through `DirectFunctionCall3`. Check limits. Copy the result into the arena. |
| `av_text_to_pgvec` | Measure the length with `strnlen`, then call `av_text_to_pgvecn`. |
| `av_pgvec_to_text` | Call the `_out` function. Copy the text into the arena. |

Errors raised by pgvector are caught with `PG_TRY`. The message comes from `av_pg_error_data()`. `FlushErrorState()` clears it.

## Ownership and thread safety

- Each call creates its own context with `AvContextCreate`, then deletes it. The result is copied out first.
- Uses the thread-local `CurrentMemoryContext`. The function restores the previous context on every path.

## Testing

`test/unit/test_text_io.c` covers accepted and rejected literals, output text and distance results. `test/unit/test_kernels.c` covers the length and bit limits.

## Notes

- Upstream error messages pass through unchanged. The code is always `invalid_vector`.
