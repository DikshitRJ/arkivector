# MCP server

`arkivectord` is a [Model Context Protocol](https://modelcontextprotocol.io) server. AI agents (Claude Code, Claude Desktop, Cursor, and any MCP client) can list your collections, run vector searches with filters, read entities, count, and, when the daemon has a wallet, write memories to the Arkiv chain. Nothing extra to install: the endpoint is built into the daemon at `POST /mcp`.

```
http://127.0.0.1:8080/mcp
```

## What an agent gets

| Tool | Does | Read-only |
| :--- | :--- | :---: |
| `arkivector_list_collections` | Collections, their vector type and dimension, index, attributes, size and sync state | yes |
| `arkivector_search` | Nearest-neighbour search with `where` / `filter`, optional documents | yes |
| `arkivector_get` | One entity by id or key | yes |
| `arkivector_count` | Count entities, optionally filtered | yes |
| `arkivector_upsert` | Write entities on chain (listed only when the daemon has a signer) | no |

Each tool runs the matching [REST endpoint](../api/README.md) inside the daemon, so behaviour, validation and errors are identical to the API. Results come back as `structuredContent` (the REST response object) plus a text block. See [Tools](tools.md) for inputs and outputs.

## Quick setup

```bash
claude mcp add --transport http arkivector http://127.0.0.1:8080/mcp     # Claude Code
```

Other clients: [Connect a client](setup.md).

## Protocol details

| | |
| :--- | :--- |
| Transport | Streamable HTTP: one JSON-RPC message per `POST /mcp`, answered with `application/json`. No server-initiated stream (`GET /mcp` → 405), no session id |
| Protocol versions | `2025-06-18` (default), `2025-03-26`, `2024-11-05` (negotiated in `initialize`) |
| Methods | `initialize`, `ping`, `tools/list`, `tools/call`; notifications are accepted with `202` |
| Auth | Same API keys as `/v1` (`Authorization: Bearer …`). Each tool call needs the scope of its REST route: a `read` key can search, writing needs `write` |
| Origin check | Requests with an `Origin` header from a non-localhost origin are rejected (`403`) unless API-key auth is on, to block DNS-rebinding attacks from web pages |

Vectors are the agent's job: `arkivector_search` takes a query **vector** of the collection's dimension. `text` queries need an embedder hook that is not configured by default; the tool says so and the agent can embed the text itself (or call your embedding API) and pass `vector`.
