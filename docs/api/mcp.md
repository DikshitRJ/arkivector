# MCP Endpoint (`POST /mcp`)

Daemon-only (capability `mcp`, phase D6). It implements the Model Context Protocol over **Streamable HTTP** (JSON-RPC 2.0 messages POSTed to `/mcp`, protocol version `2025-06-18`), so AI agents can use Arkivector as a tool server. Auth: same Bearer key as `/v1`.

## Tools

| Tool | Input schema (JSON Schema, abbreviated) | Output |
| :--- | :--- | :--- |
| `arkivector_list_collections` | `{}` | Text table + structured `{collections: [Collection]}` |
| `arkivector_search` | `{collection: string, vector?: number[], text?: string, k?: int (≤ 50), where?: string, filter?: object, include_document?: bool}` | Structured `SearchResponse`. `text` requires an embedder hook to be configured, otherwise the tool returns an error |
| `arkivector_get` | `{collection: string, ref: string, include_document?: bool}` | Structured `EntityResponse` |
| `arkivector_count` | `{collection: string, where?: string, filter?: object}` | `{count, as_of_block}` |
| `arkivector_upsert` | Present only if `writes_enabled`. `{collection, entities: EntityInput[≤ 100]}` | `UpsertResponse` |

Tool errors are returned as MCP tool results with `isError: true` and the problem object as text. The semantics are exactly those of the corresponding `/v1` endpoint.
