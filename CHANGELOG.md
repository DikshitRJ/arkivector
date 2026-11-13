# Changelog

All notable changes to Arkivector (daemon `arkivectord`, Python SDK `arkivector`, Node SDK `@dikshitrj/arkivector`). Versions follow the release tags; the REST API contract stays at `1.0.0`.

## 1.1.0 (2026-11-12)

### Daemon
- **HNSW works.** Collections created with `"index": {"method": "hnsw"}` now build and search a pgvector HNSW graph. Before, every search was an exact scan. The planner uses the graph for large candidate sets and an exact scan for small or very selective ones, and falls back to exact if a graph walk finds fewer than `k` matches.
- Approximate results report the real operator distance (L2, negative inner product, cosine) on the stored vector, sorted by distance with ties broken by key, the same as exact results.
- `ef_search`, `iterative_scan` and `max_scan_tuples` on search requests are honoured and validated (they were accepted but ignored).
- Fixed a crash on HNSW searches with `iterative_scan: "off"` and on empty graphs.
- Zero vectors under cosine are kept out of the graph (as in pgvector) and never produce NaN distances.
- Errors raised inside pgvector code no longer abort the daemon.
- Unfiltered exact searches (Qdrant and MCP endpoints) no longer score replaced vector versions.
- **MCP server works.** Every `/mcp` tool now runs the matching REST endpoint, so search returns distances, attributes and documents and accepts `where`, `filter` and `include_document`; `get` and `count` return full data; `arkivector_upsert` is available when the daemon has a signer. Results carry `structuredContent`. API-key scopes are checked per tool, `ping` and notifications (202) are handled, protocol versions are negotiated, and non-localhost `Origin` headers are rejected unless API keys are on.
- The HNSW graph pointer is read only under its lock (a benign data race found in review).

### Build and CI
- ThreadSanitizer builds disable pgvector's CPU dispatch. Its ifunc resolvers ran before the TSan runtime and crashed every TSan binary, which had kept the integration and parity suites from running in CI.
- Nightly fuzzing fits in its job timeout.
- New end-to-end HNSW integration tests (recall, distances, filters, deletes, iterative scan modes).

### Documentation
- New documentation in [`docs/wiki/`](docs/wiki/README.md), one page per topic: getting started, the REST API endpoint by endpoint (curl, requests, responses), the Python and Node.js SDKs, the MCP server, architecture, operations and development. The same pages are rendered at [arkivector.dikshitrj.in/docs](https://arkivector.dikshitrj.in/docs) and published to the [GitHub wiki](https://github.com/DikshitRJ/arkivector/wiki).
- SDK package pages (PyPI, npm) now include a quick start.

## 1.0.2, 1.0.1, 1.0.0 (2026-11-06 to 2026-11-07)

First releases: daemon with exact search, filters, writes on the Rabdi testnet, Qdrant and MCP endpoints; Python and Node SDKs; container image on GHCR.
