# daemon/src/pgvector/hnsw.h

- **Upstream:** `pgvector/src/hnsw.h`, tag v0.8.7.
- **Mode:** S (copy plus shim).
- **ARKIVECTOR deltas (inside `#ifdef ARKIVECTOR` or `#ifndef ARKIVECTOR`):**
  - `HNSW_MAX_DIM` is lifted to `VECTOR_MAX_DIM` (decision Q4, 16000 dimensions).
  - `RandomDouble()` and `SeedRandom()` use a per-thread PRNG, `av_hnsw_prng_current()`.
  - `HnswMemBuildState` and the `AvHnsw*` prototypes.
- **Phase:** D0 spike. Extended in D4.
- **Testing:** `test/unit/test_hnsw.c`. `check_provenance.py` compares the upstream part.
