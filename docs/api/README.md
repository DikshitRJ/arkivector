# Arkivector API: Daemon ↔ SDK Contract

> **Status:** frozen at spec version `1.1.0` (see [`spec/VERSION`](../../spec/VERSION)).
> **Audience:** the daemon track (implements the server) and every SDK track (implements clients).
> **Rule:** if code and this contract disagree, the **contract wins** and the code is the bug.
> Change it only through a CCR ([`spec/changes/README.md`](../../spec/changes/README.md)).

## 1. Contract artifacts

| Artifact | Kind | Purpose |
| :--- | :--- | :--- |
| [`spec/openapi.yaml`](../../spec/openapi.yaml) | Machine (OpenAPI 3.1) | Every endpoint, schema, header, and example. The source for the Prism mock and schemathesis contract tests. |
| [`spec/error-codes.json`](../../spec/error-codes.json) | Machine | Error `code` → HTTP status, retryable flag. `VB_*` VectorBytes decode errors and their check order. |
| [`spec/capabilities.json`](../../spec/capabilities.json) | Machine | Capability names advertised by `GET /v1/version`, and the daemon phase that delivers each. |
| [`spec/golden/vectorbytes.json`](../../spec/golden/vectorbytes.json) | Machine | 29 VectorBytes cases (valid + invalid). Every encoder/decoder (daemon C, SDK C/C++/Rust/Python/Node) MUST reproduce them byte-for-byte. |
| [`spec/fixtures/demo-dataset.json`](../../spec/fixtures/demo-dataset.json) | Machine | The shared `demo.docs` collection: 200 entities and 4 queries with expected exact results. The daemon seeds it; SDKs check against it. |
| [`spec/tools/gen_golden.py`](../../spec/tools/gen_golden.py) | Generator | Regenerates both golden files deterministically. Outputs are never edited by hand. |
| `docs/api/*.md` (this folder) | Human | Semantics, rules, and examples that a schema cannot express. |

## 2. Documents in this folder

| File | Covers |
| :--- | :--- |
| [conventions.md](conventions.md) | Base URL, versioning, auth, headers, JSON conventions, limits, timeouts, retries |
| [vectors.md](vectors.md) | Vector JSON forms, pgvector text literals, the **VectorBytes v1** binary format |
| [collections.md](collections.md) | Create / follow / list / get / delete collections, column and index definitions |
| [entities.md](entities.md) | Point reads, managed upsert, client-side signing (prepare + submit), delete, extend, TTL, idempotency |
| [search.md](search.md) | Search, batch search, scroll, count, distances, scores, plans |
| [filters.md](filters.md) | JSON filter AST and the Arkiv `where` language, type rules |
| [consistency.md](consistency.md) | `as_of_block`, `min_block`, sync states, read-your-writes |
| [errors.md](errors.md) | Problem format, error codes, retry rules, SDK exception mapping |
| [operations.md](operations.md) | `/healthz`, `/readyz`, `/v1/version` (capabilities), `/v1/sync`, `/metrics` |
| [qdrant.md](qdrant.md) | Qdrant-compatible subset (`/qdrant/*`). Daemon-only; SDKs don't use it |
| [mcp.md](mcp.md) | MCP endpoint (`/mcp`) for AI agents. Daemon-only |
| [sdk-guidelines.md](sdk-guidelines.md) | What every SDK MUST implement, per-language naming, mock workflow, integration milestones |
| [changelog.md](changelog.md) | Contract version history |

## 3. Endpoint summary

| Method | Path | Capability | Phase |
| :--- | :--- | :--- | :---: |
| GET | `/healthz` · `/readyz` | `ops.version` | D3 |
| GET | `/v1/version` | `ops.version` | D3 |
| GET | `/v1/sync` | `ops.sync` | D3 |
| GET | `/metrics` | `ops.metrics` | D3 |
| GET | `/v1/collections` · `/v1/collections/{name}` | `collections.read` | D3 |
| POST | `/v1/collections` | `collections.write` | D3 |
| DELETE | `/v1/collections/{name}` | `collections.write` | D3 |
| POST | `/v1/collections:follow` | `collections.follow` | D3 |
| GET | `/v1/collections/{name}/entities/{ref}` | `entities.read` | D3 |
| POST | `/v1/collections/{name}/search` | `search.exact` → `search.hnsw` | D3 → D4 |
| POST | `/v1/collections/{name}/search:batch` | `search.batch` | D4 |
| POST | `/v1/collections/{name}/scroll` | `scroll` | D3 |
| POST | `/v1/collections/{name}/count` | `count` | D3 |
| POST | `/v1/collections/{name}/entities` | `entities.write` | D5 |
| POST | `/v1/collections/{name}/entities:prepare` | `entities.prepare` | D5 |
| POST | `/v1/collections/{name}/entities:submit` | `entities.submit` | D5 (1.1) |
| POST | `/v1/collections/{name}/entities:delete` | `entities.write` | D5 |
| POST | `/v1/collections/{name}/entities:extend` | `entities.write` | D5 |
| DELETE | `/v1/collections/{name}/entities/{ref}` | `entities.write` | D5 |
| * | `/qdrant/*` | `qdrant` | D6 |
| POST | `/mcp` | `mcp` | D6 |

Phases refer to [`PLAN_DAEMON.md`](../../PLAN_DAEMON.md). Until a phase lands, the daemon returns `501 not_implemented` for that endpoint and omits the capability.

## 4. Developing against the contract before the daemon exists

```bash
# From repo root. Serves every endpoint with the examples in openapi.yaml and
# validates YOUR requests against the schemas (invalid request → 422 problem).
npx --yes @stoplight/prism-cli@5.12.0 mock -p 4010 spec/openapi.yaml
export ARKIVECTOR_URL=http://127.0.0.1:4010
```

Once the daemon reaches milestone **M2** (end of phase D3), use the real daemon seeded with the demo dataset instead:

```bash
daemon/tools/dev-up.sh          # starts mock-arkiv + arkivectord, seeds demo.docs, prints the URL
export ARKIVECTOR_URL=http://127.0.0.1:8080
```
