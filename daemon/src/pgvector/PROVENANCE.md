# PROVENANCE.md: code copied from pgvector

Upstream: <https://github.com/pgvector/pgvector>, tag **v0.8.7**, commit `f37c13f68b57d2c3472b2214fbcff699d6d34876`.
Licence: PostgreSQL License, see [`LICENSE.pgvector`](LICENSE.pgvector).
Reuse modes (ARCHITECTURE §5.1): **V** verbatim · **F** function-level copy · **S** copy + shim (`#ifndef ARKIVECTOR` guards and `/* ARKIVECTOR: … */` lines only) · **P** algorithm port.

Files in this directory build with pgvector's own flags (`-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -ffp-contract=fast`), never with our strict warning set. CI checks that **V** files are byte-identical to `pgvector/src/`.

| File | Upstream path | Mode | Functions taken | `ARKIVECTOR:` deltas | Phase |
| :--- | :--- | :---: | :--- | :--- | :---: |
| `LICENSE.pgvector` | `LICENSE` | V | — | none | D0 |
| `halfutils.c` | `src/halfutils.c` | V | whole file | none | D0 |
| `halfutils.h` | `src/halfutils.h` | V | whole file | none | D0 |
| `bitutils.c` | `src/bitutils.c` | V | whole file | none | D0 |
| `bitutils.h` | `src/bitutils.h` | V | whole file | none | D0 |
| `halfvec.h` | `src/halfvec.h` | V | whole file | none | D0 |
| `vector.h` | `src/vector.h` | V | whole file | none | D0 |
| `bitvec.h` | `src/bitvec.h` | V | whole file | none | D0 |
| `sparsevec.h` | `src/sparsevec.h` | V | whole file | none | D0 |
| `hnsw.h` | `src/hnsw.h` | S | whole file | `HNSW_MAX_DIM` → `VECTOR_MAX_DIM` (Q4); `RandomDouble()`/`SeedRandom()` use a per-thread PRNG `av_hnsw_prng_current()`; `HnswMemBuildState` + `AvHnsw*` prototypes (all under `#ifdef ARKIVECTOR`) | D0 spike |
| `hnswutils.c` | `src/hnswutils.c` | S | whole file | disk/AM-only functions wrapped in `#ifndef ARKIVECTOR` (HnswGetM, HnswGetEfConstruction, HnswOptionalProcInfo, HnswInitSupport, HnswNewBuffer, HnswInitPage, HnswInitElementFromBlock, HnswGetMetaPageInfo, HnswGetEntryPoint, HnswUpdateMetaPageInfo, HnswUpdateMetaPage, HnswSetElementTuple, HnswSetNeighborTuple, HnswLoadElementFromTuple, HnswLoadNeighborTids, HnswSearchNextBlock, HnswReadStreamNextBlock, HnswRecordPageWithFreeSpace); four of them referenced from dead `!inMemory` branches get an `#else` stub that raises ERROR (HnswLoadElementImpl, HnswLoadElement, HnswLoadUnvisitedFromDisk, HnswSearchNextBuffer). HnswGetTypeInfo and the `hnsw_*_support` type infos are compiled out until D4 (they need D1 functions). HnswInitSearchState/HnswEndSearchState are kept: no-ops for `index == NULL` on PG 17 | D0 spike |
| `hnswbuild_mem.c` | `src/hnswbuild.c` | S | AddDuplicateInMemory, FindDuplicateInMemory, AddElementInMemory, UpdateNeighborsInMemory, UpdateGraphInMemory, InsertTupleInMemory, InitGraph, InitAllocator, HnswSharedMemoryAlloc (bodies unmodified) | `#define HnswBuildState HnswMemBuildState`; exported wrappers `AvHnswInitGraph`, `AvHnswInitAllocator`, `AvHnswInsertTupleInMemory`, `AvHnswArenaAlloc` in an ARKIVECTOR section | D0 spike |
| `vector_kernels.c` | `src/vector.c` | F | (D0 part) CheckDims, VectorL2SquaredDistance, l2_distance, vector_l2_squared_distance, vector_dims, `VECTOR_TARGET_CLONES` | none | D0 spike (D1 completes) |
| `av_hnsw_glue.c` | — | ARKIVECTOR-owned | hnsw GUC variables with pgvector defaults, `HnswInitLockTranche`, per-thread PRNG, interrupt hook | — | D0 spike |

## Known upstream quirks

- `halfutils.h:93` (`HalfToFloat4`, portable path): `(bin & 0x8000) << 16` shifts a promoted `int`, which UBSan reports as `shift-base`. The value is correct on gcc/clang. `cmake/Sanitizers.cmake` disables only that check.

## Spike results (D0, decision Q3): pgvector's in-memory HNSW as Arkivector's index

Harness: `test/unit/test_hnsw.c`, suite `hnsw`. Data: 10,000 × 128-d `vector`, `vector_l2_ops`, m = 16, ef_construction = 64. The vectors are embedding-like (intrinsic dimension 16 projected into 128-d plus noise). 200 queries; recall@10 is measured against brute force. Release build (`-O2`, no `-march=native`), x86-64, 2026-11-06.

| Check | Result |
| :--- | :--- |
| recall@10 (ef_search 40 / 100 / 200) | **0.993 / 1.000 / 1.000** (≥ 0.95 required) |
| recall@10 on uniform random 128-d (worst case, informational) | 0.642 (ef 40) → 0.811 (80) → 0.929 (160) → 0.985 (320) → 0.995 (640): rises monotonically with ef |
| Insert (single thread) | 136 µs/op |
| Search only | 57 µs/query (ef 40), 218 µs/query (ef 200) |
| Arena use | 11.86 MB for 10k vectors = 1,186 B/vector (`HnswElementData` = 120 B, vector 520 B, neighbour arrays) |
| mmap relocation | arena written to a file and mapped `PROT_READ` at a different address with `av_lwlock_readonly_arena = true`: **identical** docids and distances for all 200 queries, no write faults |
| Concurrency | 4 inserters + 4 searchers on one arena: 30,000 inserts + ~84k searches in 3 s, recall 1.000. **TSan (clang) clean** in a 10 s run (12k inserts, 97k searches) |
| Seal point | a 4 MiB arena reports "seal needed" after 2,651 vectors exactly at pgvector's `memoryUsed + 1 MiB >= memoryTotal` condition; the sealed graph still has recall ≥ 0.95 |

Findings for D4:
- Searchers of a growing segment must use pgvector's own `entryWaitLock` handshake (acquire exclusive, release, then `entryLock` shared), as `InsertTupleInMemory` does. Without it the unfair 4-byte LWLock starves inserters that need `entryLock` exclusive (Debug build: 807 vs 15,127 inserts in 3 s).
- `RandomDouble()` must not use the process-global PRNG with threads: the hnsw.h delta gives each inserter thread its own state.
- No pgvector logic had to change. Only marked deltas and shims were needed, so no fallback (mode P port) is required.
