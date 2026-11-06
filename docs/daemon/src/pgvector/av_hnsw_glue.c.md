# daemon/src/pgvector/av_hnsw_glue.c

- **Upstream:** none. Arkivector-owned.
- **Purpose:** the HNSW GUC variables with pgvector's defaults, `HnswInitLockTranche`, the per-thread PRNG for `RandomDouble()`, and the interrupt hook.
- **Phase:** D0 spike.
- **Testing:** `test/unit/test_hnsw.c`.
