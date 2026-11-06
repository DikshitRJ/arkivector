# Proposal: daemon-d0-foundations

## Why
The daemon (`arkivectord`) reuses pgvector v0.8.7 code (ARCHITECTURE §5). Before later phases build on this, we need three things: a build that compiles copied pgvector and PostgreSQL files unmodified against a shim layer; a deterministic Arkiv node simulator for every later integration test; and proof (decision Q3) that pgvector's in-memory HNSW graph works as a long-lived, mmapped, concurrently searched index.

## What changes
- CMake build with strict warnings for our code and pgvector's own flags for copied code. Presets: dev, release, asan, ubsan, tsan, fuzz, release-static.
- Vendored third-party libraries, pinned with sha256 values.
- `pgcompat`: PostgreSQL API shims (palloc on arenas, ereport/PG_TRY, LWLock, fmgr, …) plus verbatim PostgreSQL helper files.
- Verbatim pgvector kernels (halfutils, bitutils, type headers).
- The HNSW in-memory spike: copied hnsw.h/hnswutils.c/hnswbuild insertion with marked deltas, and a test harness.
- mock-arkiv v0 (Python) with conformance tests.
- CI (two phases), PR template, editor/format configuration.

## Capabilities
- `daemon-build`: build, shim layer and copied-code integrity.
- `mock-arkiv`: deterministic Arkiv node simulator used by tests.
- `hnsw-arena-index`: pgvector HNSW graphs in position-independent arenas.

## Out of scope
Sync, HTTP, filters and search APIs (D2–D3). Kernel tiers and VectorBytes (D1). HNSW segments in the server (D4). Rabdi measurements (D6, decision Q5).

## Impact
New code under `daemon/`, `.github/workflows/{daemon,contract}.yml`, root `.clang-format` and `.editorconfig`. No contract (`spec/`) changes.
