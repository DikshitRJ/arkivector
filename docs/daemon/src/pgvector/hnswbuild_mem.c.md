# daemon/src/pgvector/hnswbuild_mem.c

- **Upstream:** `pgvector/src/hnswbuild.c`, tag v0.8.7. Only the in-memory functions are copied.
- **Mode:** S (copy plus shim).
- **Copied functions:** `AddDuplicateInMemory`, `FindDuplicateInMemory`, `AddElementInMemory`, `UpdateNeighborsInMemory`, `UpdateGraphInMemory`, `InsertTupleInMemory`, `InitGraph`, `InitAllocator`, `HnswSharedMemoryAlloc`. The bodies are unchanged.
- **ARKIVECTOR deltas:**
  - `#define HnswBuildState HnswMemBuildState`.
  - Exported wrappers `AvHnswInitGraph`, `AvHnswInitAllocator`, `AvHnswInsertTupleInMemory` and `AvHnswArenaAlloc`, inside an `ARKIVECTOR` section.
- **Phase:** D0 spike. Used by the insert path in D4.
- **Testing:** `check_provenance.py` compares each copied function with upstream. `test/unit/test_hnsw.c` covers the insert path.
