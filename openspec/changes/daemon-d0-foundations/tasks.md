# Tasks: daemon-d0-foundations

## 1. Build
- [x] 1.1 CMake skeleton, presets, Warnings/Sanitizers modules
- [x] 1.2 Vendor yyjson, CRoaring, llhttp, LMDB, tomlc99, greatest, libsecp256k1, tiny_sha3 (VENDORED.lock)
- [x] 1.3 Bump arena allocator

## 2. pgcompat and pgvector
- [x] 2.1 c.h/postgres.h types, palloc on arenas, ereport/PG_TRY
- [x] 2.2 Verbatim halfutils/bitutils and type headers
- [x] 2.3 Unit tests: pgcompat, half/bit kernels

## 3. HNSW spike
- [x] 3.1 LWLock/spin/itemptr/fmgr shims, verbatim list/pairingheap/simplehash/relptr/pg_prng
- [x] 3.2 Copy hnsw.h, hnswutils.c, hnswbuild in-memory functions with marked deltas
- [x] 3.3 Spike harness: recall, relocation, concurrency (TSan), seal point

## 4. Test infrastructure
- [x] 4.1 mock-arkiv v0
- [x] 4.2 mock-arkiv conformance tests

## 5. CI and hygiene
- [ ] 5.1 daemon.yml (two phases), contract.yml, PR template
- [x] 5.2 .clang-format, .editorconfig, .gitignore
- [x] 5.3 OpenSpec init and this change
