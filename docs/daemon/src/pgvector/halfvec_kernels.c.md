# daemon/src/pgvector/halfvec_kernels.c

- **Upstream:** `pgvector/src/halfvec.c`, tag v0.8.7.
- **Mode:** F (function-level copy), tiered.
- **Functions:** `CheckDims`, `halfvec_l2_distance`, `halfvec_l2_squared_distance`, `halfvec_inner_product`, `halfvec_negative_inner_product`, `halfvec_cosine_distance`, `halfvec_l1_distance`.
- **Tiers:** baseline, `_t3` and `_t4`.
- **ARKIVECTOR deltas:** none.
- **Performance note:** the F16C kernel uses one accumulator. `bench/kernels_bench.c` measures an alternative with four accumulators. The result differs in the last bits, so it is not used in the daemon.
- **Testing:** `test/unit/test_kernels.c`, with a tolerance of 2e-3 for halfvec.
