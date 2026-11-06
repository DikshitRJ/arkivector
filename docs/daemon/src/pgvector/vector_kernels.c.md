# daemon/src/pgvector/vector_kernels.c

- **Upstream:** `pgvector/src/vector.c`, tag v0.8.7.
- **Mode:** F (function-level copy), tiered.
- **Functions:** `CheckDims`, `VectorL2SquaredDistance`, `l2_distance`, `vector_l2_squared_distance`, `VectorInnerProduct`, `inner_product`, `vector_negative_inner_product`, `VectorCosineSimilarity`, `cosine_distance`, `VectorL1Distance`, `l1_distance`.
- **Tiers:** compiled as baseline, `_t3` and `_t4` (see `cmake/KernelTiers.cmake`). The `_t3` and `_t4` suffixes come from `av_tier.h`.
- **ARKIVECTOR deltas:** none. The file is unchanged.
- **Testing:** `test/unit/test_kernels.c`, `tiered_float_kernels_match_reference`.
