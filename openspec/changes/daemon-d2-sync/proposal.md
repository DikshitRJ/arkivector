# Proposal: daemon-d2-sync

## Why
The daemon must mirror Arkiv collections into local storage exactly (bootstrap, live, TTL, reorg, reconcile, restart) and serve operational endpoints. Phases D3–D6 build on that.

## What changes
Runtime (config, logging, thread pool, mpsc), the libuv/llhttp HTTP server with ops endpoints and metrics, the blocking Arkiv JSON-RPC/WS client, LMDB storage and flat segments, the collection runtime and registry, and the sync engine. See design.md for the lane contracts, and for the decision to use blocking libcurl on a dedicated sync thread.

## Capabilities
- `chain-sync`: mirroring Arkiv state into the daemon.

## Out of scope
The read API, filters and search (D3), HNSW (D4), writes (D5).
