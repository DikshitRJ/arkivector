# REST API

Everything Arkivector does is a JSON-over-HTTP call to the daemon, so any language (or a shell) can use it without an SDK. These pages document every endpoint with `curl`, its request fields and its response. Responses shown were captured from a real daemon.

## Conventions

| | |
| :--- | :--- |
| Base URL | `http://127.0.0.1:8080` (wherever `arkivectord` listens) |
| Request bodies | `Content-Type: application/json`, UTF-8. Unknown fields are rejected (`400 bad_request`), so typos fail loudly |
| Responses | `application/json`; errors are `application/problem+json` ([Errors](errors.md)) |
| Auth | Off by default. With an [API-key file](../operations/security.md), send `Authorization: Bearer <key>` |
| Block | Data responses carry `as_of_block` and the `Arkivector-As-Of-Block` header ([Consistency](consistency.md)) |
| Request id | Send `X-Request-Id` (≤ 64 chars) or get a generated one back; it is echoed in errors |
| Field names | `snake_case`. Keys are lowercase hex; addresses come back in EIP-55 case |
| Limits | Body 32 MiB, `k` ≤ 1000, `offset + k` ≤ 1000, 1000 entities per write, 64 searches per batch |

The examples use:

```bash
export AV=http://127.0.0.1:8080
```

## Endpoints

| Method | Path | Page |
| :--- | :--- | :--- |
| GET | `/healthz`, `/readyz` | [Health and version](health.md) |
| GET | `/v1/version`, `/v1/sync`, `/metrics` | [Health and version](health.md) |
| GET, POST | `/v1/collections` | [Collections](collections.md) |
| GET, DELETE | `/v1/collections/{name}` | [Collections](collections.md) |
| POST | `/v1/collections:follow` | [Collections](collections.md) |
| POST | `/v1/collections/{name}/entities` | [Upsert entities](entities-upsert.md) |
| GET | `/v1/collections/{name}/entities/{ref}` | [Get an entity](entities-get.md) |
| DELETE | `/v1/collections/{name}/entities/{ref}` | [Delete and extend](entities-delete-extend.md) |
| POST | `/v1/collections/{name}/entities:delete` | [Delete and extend](entities-delete-extend.md) |
| POST | `/v1/collections/{name}/entities:extend` | [Delete and extend](entities-delete-extend.md) |
| POST | `/v1/collections/{name}/entities:prepare` | [Prepare](entities-prepare.md) |
| POST | `/v1/collections/{name}/search` | [Search](search.md) |
| POST | `/v1/collections/{name}/search:batch` | [Batch search](search-batch.md) |
| POST | `/v1/collections/{name}/scroll` | [Scroll](scroll.md) |
| POST | `/v1/collections/{name}/count` | [Count](count.md) |
| POST | `/mcp` | [MCP server](../mcp/README.md) |
| * | `/qdrant/*` | [Qdrant compatibility](qdrant.md) |

Shared request pieces: [Filters](filters.md), [Vectors](vectors.md), [Consistency](consistency.md).

## Capabilities

`GET /v1/version` lists what this daemon supports (`capabilities`). Writes need a signer (`writes_enabled: true`); a feature that is absent returns `501 not_implemented`. The SDKs check capabilities before calling.

## Machine-readable contract

[`spec/openapi.yaml`](https://github.com/DikshitRJ/arkivector/blob/main/spec/openapi.yaml) (OpenAPI 3.1) describes every endpoint and schema; [`spec/error-codes.json`](https://github.com/DikshitRJ/arkivector/blob/main/spec/error-codes.json) lists error codes. You can mock the API without a daemon:

```bash
npx --yes @stoplight/prism-cli@5.12.0 mock -p 4010 spec/openapi.yaml
```
