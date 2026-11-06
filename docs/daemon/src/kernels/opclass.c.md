# daemon/src/kernels/opclass.c

## Purpose

Implements the opclass registry in `opclass.h`.

## Key parts

- `call2` compares the two dimensions. On a mismatch it returns NaN. Otherwise it calls the dispatched kernel through `DirectFunctionCall2`.
- `THUNK` and `THUNKS` build one wrapper per type and metric.
- `OC_FLOAT` builds the four entries for `vector`, `halfvec` and `sparsevec`: `_l2_ops`, `_ip_ops`, `_cosine_ops` and `_l1_ops`.
- `registry` lists the 14 entries: 12 float entries and two bit entries (`bit_hamming_ops`, `bit_jaccard_ops`).

## Ownership and thread safety

The registry is `static const`. Lookups are linear and read-only. Reentrant.

## Testing

`test/unit/test_kernels.c`, test `opclass_registry_matches_pgvector`. It checks the index metrics, the normalize flag and the 3-4-5 distance.

## Notes

- `vector_cosine_ops` and similar use the negative inner product as the index distance, like pgvector.
