# Connect a client

The daemon must be running ([Installation](../getting-started/installation.md)). The MCP URL is the daemon URL plus `/mcp`, e.g. `http://127.0.0.1:8080/mcp`. If the daemon uses API keys, give the client a key: a `read` key for search-only agents, a `write` key if the agent may store memories.

## Claude Code

```bash
claude mcp add --transport http arkivector http://127.0.0.1:8080/mcp
```

With an API key:

```bash
claude mcp add --transport http arkivector http://127.0.0.1:8080/mcp --header "Authorization: Bearer <key>"
```

Check it with `claude mcp list`, or `/mcp` inside a session.

## Claude Desktop

Claude Desktop talks to local servers over stdio, so bridge the HTTP endpoint with [`mcp-remote`](https://www.npmjs.com/package/mcp-remote) (needs Node.js). In `claude_desktop_config.json` (Settings → Developer → Edit Config):

```json
{
  "mcpServers": {
    "arkivector": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "http://127.0.0.1:8080/mcp", "--transport", "http-only"]
    }
  }
}
```

With an API key, add `"--header", "Authorization: Bearer <key>"` to `args`. Restart Claude Desktop.

## Cursor

`~/.cursor/mcp.json` (or `.cursor/mcp.json` in a project):

```json
{
  "mcpServers": {
    "arkivector": {
      "url": "http://127.0.0.1:8080/mcp",
      "headers": { "Authorization": "Bearer <key>" }
    }
  }
}
```

Leave out `headers` when auth is off.

## VS Code (GitHub Copilot agent mode)

`.vscode/mcp.json`:

```json
{
  "servers": {
    "arkivector": { "type": "http", "url": "http://127.0.0.1:8080/mcp" }
  }
}
```

## Any MCP client

Point a Streamable HTTP client at the URL. With the official Python SDK:

```python
import asyncio
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client

async def main():
    async with streamable_http_client("http://127.0.0.1:8080/mcp") as (read, write):
        async with ClientSession(read, write) as s:
            await s.initialize()
            print([t.name for t in (await s.list_tools()).tools])
            res = await s.call_tool("arkivector_search", {"collection": "notes", "vector": [1, 0, 0, 0], "k": 3})
            print(res.structured_content["results"])

asyncio.run(main())
```

(`streamable_http_client` is the name in `mcp` 2.x; 1.x calls it `streamablehttp_client` and yields three values.)

Or with plain HTTP:

```bash
curl -s http://127.0.0.1:8080/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"arkivector_list_collections","arguments":{}}}'
```

## Remote daemons

To use a daemon on another machine, put it behind HTTPS (a reverse proxy, see [Deployment](../operations/deployment.md)), **enable API keys**, and use `https://your-host/mcp` with a `Bearer` header. Without API keys the daemon refuses browser origins other than localhost.

## Troubleshooting

| Symptom | Fix |
| :--- | :--- |
| `401` | The daemon has API keys: add the `Authorization` header |
| `403 forbidden` on a tool | The key lacks the scope (writes need `write`) |
| `403` before any tool runs | A browser-based client sent a non-localhost `Origin`: enable API keys or run the client locally |
| `arkivector_upsert` missing | The daemon has no `ARKIV_PRIVATE_KEY` |
| Search returns `dimension_mismatch` | The query vector must have the collection's dimension (see `arkivector_list_collections`) |
