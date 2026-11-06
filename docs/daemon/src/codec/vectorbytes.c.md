# daemon/src/codec/vectorbytes.c

## Purpose

Implements the VectorBytes v1 codec in `vectorbytes.h`. The file is the only parser of on-chain vector bytes.

## Validation order

`av_vb_validate` checks the input in this order. It stops at the first failure and sets `vb_error`.

1. Length of at least 8 bytes. Sparsevec needs 16 bytes after the type is known. Otherwise `VB_TOO_SHORT`.
2. Magic `AV`. Otherwise `VB_BAD_MAGIC`.
3. Format version 1. Otherwise `VB_BAD_VERSION`.
4. Type nibble 1 to 4. Otherwise `VB_UNKNOWN_TYPE`.
5. Reserved flag bits are zero. Otherwise `VB_BAD_FLAGS`.
6. Unused fields are zero. Otherwise `VB_UNUSED_NONZERO`.
7. Dimension in range. Otherwise `VB_DIM_OUT_OF_RANGE`.
8. Sparsevec `nnz` in range. Otherwise `VB_NNZ_OUT_OF_RANGE`.
9. Exact length. Otherwise `VB_LENGTH_MISMATCH`.
10. Payload checks: no NaN or infinity (`VB_NON_FINITE`), zero padding bits in bit vectors (`VB_BIT_PADDING`), strictly ascending sparse indices (`VB_SPARSE_UNSORTED`), indices in range (`VB_SPARSE_INDEX_OUT_OF_RANGE`), and no explicit zero values (`VB_SPARSE_ZERO_VALUE`).

## Key functions

| Function | Behavior |
| :--- | :--- |
| `av_vb_encoded_size` | Compute the length from the type and dimension. Returns false outside the limits. |
| `av_vb_validate` | Run the checks above. Read fields with explicit little-endian helpers. |
| `av_vb_decode` | Validate. Allocate `len` bytes with 8-byte alignment. Copy from byte 4 on. Set the varlena header. |
| `av_vb_encode` | Copy the struct payload behind a new header, then validate the new blob. |

## Ownership and thread safety

- Results belong to the caller's arena.
- Uses no shared state. Reentrant.
- The file includes a compile-time check that the host is little-endian.

## Testing

- `test/unit/test_vectorbytes.c`, suite `vectorbytes`, runs the golden cases.
- `test/fuzz/fuzz_vectorbytes.c` runs the same checks on random input.

## Notes

- The struct copy assumes a little-endian host, the same as pgvector.
