# `daemon/src/server/mcp.c`

Implements the Model Context Protocol (MCP) Streamable HTTP transport (protocol version `2025-06-18`).

## Supported JSON-RPC 2.0 Methods
- `initialize`: Returns server info (`arkivector`), protocol version, and tool capabilities.
- `notifications/initialized`: Confirms agent connection (returns 204 No Content).
- `tools/list`: Advertises available tools:
  - `arkivector_list_collections`: Lists vector collections and dimensions.
  - `arkivector_search`: Vector similarity search with optional query vector, limit, and SQL-like `where` filter.
  - `arkivector_get`: Point lookup by external id or hex key.
  - `arkivector_count`: Point count with optional `where` filter.
- `tools/call`: Executes the requested tool and packages results or errors into MCP tool content envelopes.
