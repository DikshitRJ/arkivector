# daemon/src/pgvector/sparsevec_kernels.c

- **Upstream:** `pgvector/src/sparsevec.c`, tag v0.8.7.
- **Mode:** F (function-level copy), tiered.
- **Functions:** `CheckDims`, `SparsevecL2SquaredDistance`, `sparsevec_l2_distance`, `sparsevec_l2_squared_distance`, `SparsevecInnerProduct`, `sparsevec_inner_product`, `sparsevec_negative_inner_product`, `sparsevec_cosine_distance`, `sparsevec_l1_distance`.
- **Tiers:** baseline, `_t3` and `_t4`.
- **ARKIVECTOR deltas:** none.
- **Testing:** `test/unit/test_kernels.c`, with up to 200 non-zero entries per vector.
