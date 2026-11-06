# daemon/src/pgvector/hnswutils.c

- **Upstream:** `pgvector/src/hnswutils.c`, tag v0.8.7.
- **Mode:** S (copy plus shim).
- **ARKIVECTOR deltas:** disk and access-method functions are wrapped in `#ifndef ARKIVECTOR`. Some call sites in dead disk branches get an `#else` stub that raises `ERROR`. The stubs are `HnswLoadElementImpl`, `HnswLoadElement`, `HnswLoadUnvisitedFromDisk` and `HnswSearchNextBuffer`. The full list is in `src/pgvector/PROVENANCE.md`.
- **Kept:** `HnswInitSearchState` and `HnswEndSearchState`. They are no-ops for `index == NULL`.
- **Phase:** D0 spike. In-memory graph search and neighbour selection.
- **Testing:** `test/unit/test_hnsw.c`.
