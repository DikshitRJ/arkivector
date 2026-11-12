# Tasks: daemon-hnsw-ci-fix

Branch `claude/busy-babbage-k4puq6` (from `main` @ 8cff8dd). Legend: [x] done · [ ] open.

## 1. CI
- [x] 1.1 Diagnose the TSan segfault with gdb (ifunc resolver before TSan init), not the kernel
- [x] 1.2 `cmake/Sanitizers.cmake`: `DISABLE_DISPATCH` for TSan builds; 18/18 TSan unit tests pass with ASLR on
- [x] 1.3 `daemon.yml`: drop `setarch -R`; nightly fuzz 300 s/target, 50 min timeout
- [x] 1.4 Run every Phase 2 step locally: ASan 18/18, TSan 18/18, mock selftest 29/29, integration, parity 97/97
      (PostgreSQL 16 + pgvector 0.8.7 built from `pgvector/`; Docker Hub was rate limited in the sandbox)
- [x] 1.5 First green `daemon` run on GitHub (run 38, commit d5ea184)

## 2. HNSW
- [x] 2.1 `hnswscan_mem.c`: NULL discarded heap (off mode, empty graph); strict_order guard from −∞ (B4, B9)
- [x] 2.2 `segment.c`: PG_TRY around insert/search; `HnswFormIndexValue` (zero-norm → `AV_SKIPPED`); zero query → no hits
- [x] 2.3 `av_segment_hnsw_reserve` (type-aware capacity)
- [x] 2.4 Collections build the graph at start and per insert; exact tail; rebuild after publish (C1)
- [x] 2.5 `av_collection_search_hnsw`: true operator distances (B1), raw vectors kept for exact paths (B2),
      key tie-break (B6), `ef_search >= offset + k` and iterative bump on `offset + k` (B8)
- [x] 2.6 Executor: exact fallback when the walk is short; unfiltered flat scans score live docids only
- [x] 2.7 Planner: unfiltered candidate set → `hnsw`, filtered → `hnsw_iterative`
- [x] 2.8 REST: parse/validate `ef_search`, `iterative_scan`, `max_scan_tuples` (single + batch)
- [x] 2.9 `av_compact_rebuild` rebuilds the graph (flat segment never replaced)

## 3. Tests
- [x] 3.1 Unit: off/relaxed/strict on empty and filled graphs (crashes on the old scan), cosine zero-norm
- [x] 3.2 Integration `test_hnsw.py` (6 tests): plan, recall@10 ≥ 0.9, L2/ip distances, filtered iterative
      modes, zero vectors, deletes/updates, knob validation. 171/171 integration tests pass
- [x] 3.3 HNSW/search/Qdrant/MCP integration tests against the ASan daemon binary: clean
- [x] 3.4 clang-format 23.1.4, clang-tidy, cppcheck, check_provenance.py clean
- [x] 3.6 Review (code-review skill; the gemini-reviewer backend is not available here): fixed writer
      starvation (writer-preferring hnsw_lock), GNU qsort_r portability, dead-ratio overestimate, batch error
      leak, duplicated rebuild code
- [ ] 3.5 CI green on the PR head

## 3b. Graphify review of #8 (2026-11-12, after merge; fixed on the follow-up branch)
Graphify posted 5 advisory findings (check run "Graphify", review marked partial) and 7 inline
"health regression" notes. Each was checked against the code:
- [x] allocatorLock held across an ereport inside PG_TRY (segment.c av_hnsw_insert): **not a bug**.
      LWLockAcquire records every lock in a per-thread held list; PG_TRY saves its depth and PG_CATCH
      calls av_pg_unwind_restore, which releases every lock taken since (pgcompat lwlock.c release_to).
      Unit test test_pgcompat.c error_releases_lwlocks covers exactly this pattern.
- [x] c->hnsw read without hnsw_lock in av_collection_has_hnsw, the early check of
      av_collection_search_hnsw and av_collection_get_stats: **real (benign) data race, fixed**. New
      immutable `hnsw_enabled` flag, set in create before the collection is shared; lock-free readers use
      it, and the graph pointer is read by query threads only under hnsw_lock. Applier-side reads stay
      unlocked (the applier is the only writer).
- [x] NULL candidate bitmap with a non-empty tail: **false positive**. av_collection_search_hnsw replaces
      a NULL cand with the live bitmap before taking the lock or touching the tail.
- [x] NULL opclass dereference in av_collection_search_hnsw: **false positive**. A graph is built only when
      def.opclass is set (create_opts), so oc falls back to a non-NULL column opclass whenever a graph exists.
- [x] 7 inline notes (hnsw_build, av_collection_search_hnsw, av_segment_create_hnsw, parse_hnsw_knobs and
      three test functions "fan out to 6-12 callees"): coupling metrics, not defects. No change.
- [x] The formal-verification check abstained on all 14 changed functions (unsupported types): nothing to act on.

## 4. Follow-ups (not in this change)
- [ ] Persist sealed graphs (restart currently rebuilds the graph from LMDB)
- [ ] Rebuild on a background thread instead of the applier
- [ ] Batch search: reject invalid searches instead of skipping them (search.md: whole batch fails)
- [ ] Contract tests (schemathesis) in CI
- [ ] Planner: expired-but-unswept rows make unfiltered REST plans report hnsw_iterative (results are correct)
- [ ] Share one (distance, key, NaN-last) comparator between flat.c, hnswscan_mem.c and collection.c
