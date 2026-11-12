# Proposal: daemon-hnsw-ci-fix

## Why
1. **CI never ran the integration tests.** On every push and nightly run since 2026-11-06, the
   `Phase 2: integration tests` job died at `Unit tests (TSan)`: all 18 TSan binaries segfaulted before
   `main()`. Every later step (dev build, mock-arkiv selftest, the 165 integration tests, the pgvector
   parity suite) was skipped. Locally the suites passed, because nobody ran TSan locally. The nightly
   fuzz job was cancelled every night (7 targets × 600 s > 60 min timeout).
2. **HNSW did not work.** `"index": {"method": "hnsw"}` was accepted and reported, but no collection
   ever built a graph: collections always created a flat segment and `av_compact_rebuild` (the only
   code that created HNSW segments) had no caller. The planner's HNSW branch was unreachable. Once
   reached, the HNSW scan crashed (`iterative_scan = off`, empty graphs), reported pgvector's index
   distances (L2², −ip) as API distances, and the REST layer ignored `ef_search`, `iterative_scan`
   and `max_scan_tuples`.

## What changes
- TSan builds define pgvector's `DISABLE_DISPATCH` (root cause: ifunc resolvers run before the TSan
  runtime). CI drops the `setarch -R` workaround; nightly fuzzing fits the timeout.
- HNSW scan fixes (NULL discarded heap, strict-order guard from −∞).
- HNSW segment: pgvector errors caught with `PG_TRY` (no daemon abort), zero-norm vectors skipped like
  upstream, type-aware capacity estimate.
- Collections with an hnsw index keep a pgvector graph next to the flat segment (source of truth),
  with an exact tail, rebuilds after publish, true operator distances, key tie-break, and exact
  fallback.
- REST search parses and validates the three pgvector knobs.
- New unit regressions and an end-to-end HNSW integration suite.

## Capabilities
- `hnsw-search`: approximate search through a real pgvector HNSW graph.

## Out of scope
Persisting sealed graphs across restarts (the graph is rebuilt from LMDB at start, like the flat
segment), multi-segment expiry bands and a background compactor thread (C2), contract tests in CI.
