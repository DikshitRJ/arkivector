# daemon/src/pgvector/vector_funcs.c

- **Upstream:** `pgvector/src/vector.c`, tag v0.8.7.
- **Mode:** F (function-level copy).
- **Functions:** `CheckExpectedDim`, `CheckDim`, `vector_dims`, `vector_norm`, `l2_normalize`, `binary_quantize`, `subvector`, `halfvec_to_vector`.
- **ARKIVECTOR deltas:** none.
- **Testing:** `test/unit/test_text_io.c` covers the parts that the API uses. The rest is untested in D1.
