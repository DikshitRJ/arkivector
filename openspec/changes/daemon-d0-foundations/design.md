# Design: daemon-d0-foundations

## Build layering
`av_base` (arena) ← `av_pgcompat` (shims + verbatim PostgreSQL files) ← `av_pgvector` (copied pgvector, upstream flags) ← `av_core` (our code, strict) ← `arkivectord`, `av_unit`. Configure-time facts are generated into `pg_config.h` (PG_VERSION_NUM 170011, BLCKSZ 8192, cpuid/xgetbv/builtins).

## pgcompat decisions
- MemoryContext = context tree over `av_arena`, with 16-byte chunk headers so `GetMemoryChunkContext`/`repalloc` work. `pfree` is a no-op.
- PG_TRY uses `_setjmp`/`_longjmp` (equivalent to `sigsetjmp(buf, 0)`): ThreadSanitizer cannot follow `sigsetjmp` with glibc 2.44. Before unwinding, `pg_re_throw` calls `LWLockReleaseAll()`, mirroring PostgreSQL abort processing.
- LWLock = 4-byte atomic word (bit 31 exclusive, low bits shared count) plus a per-thread held-lock stack. A thread-local read-only flag turns acquire/release into no-ops for sealed `PROT_READ` arenas.
- ItemPointerData keeps PostgreSQL's 6-byte layout. It holds our docid as block number (offset 1).

## HNSW deltas
Only `ARKIVECTOR`-marked changes: `HNSW_MAX_DIM` = `VECTOR_MAX_DIM`; a per-thread level PRNG (pgvector assumes one process per backend); a `HnswMemBuildState` subset; disk/AM functions under `#ifndef ARKIVECTOR`, with ERROR stubs where dead in-memory branches reference them. The arena layout puts the `HnswGraph` header at offset 0, so the whole index is one relocatable blob. Allocation uses pgvector's `HnswSharedMemoryAlloc` bump allocator.

## Concurrency finding
Searchers of a growing graph take pgvector's `entryWaitLock` handshake before `entryLock` shared. Otherwise the unfair LWLock starves inserters that need the entry lock exclusively.

## Alternatives considered
- A pthread_rwlock-based LWLock: rejected. At 56 bytes it would bloat every `HnswElementData`.
- Porting HNSW into our own structs (mode P): not needed, because the spike passed.
