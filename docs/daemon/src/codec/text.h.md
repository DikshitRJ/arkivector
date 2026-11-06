# daemon/src/codec/text.h

## Purpose

Converts pgvector text literals to pgvector structs, and back. The parsing uses pgvector's own `*_in` and `*_out` functions. Accepted inputs and error messages therefore match pgvector exactly.

## Public API

| Function | Purpose |
| :--- | :--- |
| `av_status av_text_to_pgvec(av_vtype t, const char *s, int32_t typmod_dim, av_arena *a, void **out, av_err *e)` | Parse a NUL-terminated literal. |
| `av_status av_text_to_pgvecn(av_vtype t, const char *s, size_t len, int32_t typmod_dim, av_arena *a, void **out, av_err *e)` | Parse `len` bytes. Use this for JSON strings. |
| `av_status av_pgvec_to_text(av_vtype t, const void *v, av_arena *a, char **out, av_err *e)` | Format a struct with pgvector's `*_out`. The result is NUL-terminated. |
| `size_t av_pgvec_size(const void *v)` | The varlena length of a struct. |

Constants: `AV_TEXT_MAX_LEN` is 1 MiB. `AV_TEXT_CTX_CAP` is 16 MiB, the cap of the per-call memory context.

`typmod_dim` greater than 0 requires that exact dimension, as the SQL typmod does. Pass -1 for no check.

## Behavior

- Literals longer than `AV_TEXT_MAX_LEN` fail with `invalid_vector`. So do literals with an embedded NUL.
- Parsed values must also satisfy the VectorBytes limits. This bounds memory per input.
- A pgvector `ereport(ERROR)` is caught and its message is returned in `e->msg`.

## Ownership and thread safety

- Output is allocated in the caller's arena.
- Each call creates and deletes a private memory context. The functions are reentrant.

## Testing

`test/unit/test_text_io.c` ports the literal and output cases of pgvector's regression files. `test/fuzz/fuzz_vector_in.c` checks that accepted text re-parses to the same struct.

## Notes

- The code `invalid_vector` is used for all text errors, including out-of-memory cases caught by PostgreSQL code.
