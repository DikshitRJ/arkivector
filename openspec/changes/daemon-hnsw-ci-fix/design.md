# Design: daemon-hnsw-ci-fix

## 1. TSan startup segfault (root cause)
`gdb` on a TSan unit binary (clang 18.1.3, same as the ubuntu-24.04 runner):
```
#0 __tsan_func_entry ()
#1 VectorCosineSimilarity.resolver ()
#2 elf_machine_lazy_rel ... _dl_relocate_object ... dl_main   (ld.so, before main)
```
pgvector's kernels use `__attribute__((target_clones("default", "fma")))` (`vector_kernels.c`,
`bitutils.c`), which compiles to GNU ifuncs. glibc runs ifunc resolvers during relocation, before
the TSan runtime initialises; the resolver is instrumented and faults. `vm.mmap_rnd_bits` and
`setarch -R` (the earlier workaround) are unrelated, which is why the job kept failing.
**Fix:** `cmake/Sanitizers.cmake` adds `-DDISABLE_DISPATCH` for `AV_TSAN` (pgvector's own switch in
`halfvec.h`). Upstream files are untouched; TSan exercises the portable kernels.

## 2. HNSW as a side index of the flat segment
```
             applier thread                             query workers
 upsert ──► flat segment (raw vectors, source of truth) ◄── exact scans, distance recompute
        └─► hnsw graph (pgvector in-memory build)       ◄── GetScanItems / ResumeScanItems
              └─ hnsw_tail bitmap (not in graph)        ◄── exact scan, merged
```
- **Why not replace the flat segment** (the old `av_compact_rebuild` design): cosine graphs store
  normalized values, so exact scans and non-index operators would see different numbers (B2); a
  sealed segment cannot take inserts; and every lookup (`include: ["vector"]`, rerank) would need
  two code paths.
- **Locking.** `hnsw_lock` (rwlock) guards `hnsw`, `hnsw_tail`, `hnsw_full`. The applier holds it
  exclusively for one graph insert (sub-millisecond) and for the pointer swap after a rebuild;
  readers hold it shared for one search. Order: collection read lock, then `hnsw_lock`. Unpublished
  docids are in the graph but never in the candidate bitmap (live ∩ filter), so readers cannot see them.
- **Capacity.** The graph reserves address space for `max(65536, 2N)` vectors (`MAP_NORESERVE`,
  committed on use). When pgvector's memory check says full (`AV_SEAL_NEEDED`), new docids go to the
  tail. Zero-norm cosine vectors (`AV_SKIPPED`) and insert errors go to the tail as well.
- **Rebuild** (applier, after publish): when the tail exceeds `max(1024, 10%)` of a full graph or more
  than 30% of the graph is dead. The build reads the flat segment and the published live bitmap
  (only the applier mutates either), then swaps under `hnsw_lock`.
- **Distances.** The graph returns index distances (L2², −ip, cosine on normalized values). Hits are
  rescored with the requested operator on the raw vectors, sorted by `(distance, key)` (search.md
  tie-break), then cut to `offset + k`. `ef_search` is raised to at least `offset + k` (cap 1000).
- **Fallback.** If the walk returns fewer than `offset + k` of the candidates (e.g. `iterative_scan:
  off` under a selective filter), the executor reruns the query as an exact flat scan and reports
  `exact_flat`.

## 3. Errors inside pgvector code
`ereport(ERROR)` outside a `PG_TRY` frame calls `abort()` (`pgcompat/src/elog.c`). HNSW insert and
search now run inside `PG_TRY` with a private memory context and return `AV_ERR` with the message.
