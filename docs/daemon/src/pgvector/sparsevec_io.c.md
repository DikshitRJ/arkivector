# daemon/src/pgvector/sparsevec_io.c

- **Upstream:** `pgvector/src/sparsevec.c`, tag v0.8.7.
- **Mode:** F (function-level copy).
- **Functions:** `palloc_array_checked` block, `SparseInputElement`, `CheckExpectedDim`, `CheckDim`, `CheckNnz`, `CheckIndex`, `CheckElement`, `InitSparseVector`, `AppendElement`, `FinishAppend`, the `sparsevec_isspace` block, `CompareIndices`, `sparsevec_in`, the `AppendChar` macro, `sparsevec_out`.
- **ARKIVECTOR deltas:** none.
- **Testing:** `test/unit/test_text_io.c`. It covers the index and nnz error messages.
