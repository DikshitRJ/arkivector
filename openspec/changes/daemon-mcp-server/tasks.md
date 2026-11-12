# Tasks: daemon-mcp-server

- [x] 1. Verify the old server with the official MCP Python SDK (mcp 2.3.0, streamable HTTP client):
      handshake worked, tools were incomplete (see proposal)
- [x] 2. http: capture Origin; av_http_auth(); REST av_rest_set_auth / av_rest_get_auth / av_rest_writes_enabled
- [x] 3. mcp.c rewrite: router dispatch, per-call scope checks, structuredContent, upsert tool, annotations,
      instructions, ping, 202, version negotiation, batch rejection, Origin check
- [x] 4. Official client end to end: upsert → list → search (where/filter/documents) → count → get, errors as isError
- [x] 5. test_mcp.py: protocol, transport, Origin, tools with a signer, read key cannot upsert (3 tests)
- [x] 6. Full gate: clang-format, clang-tidy, cppcheck, unit dev/ASan/TSan 18/18, integration 173/173
- [ ] 7. stdio bridge (`npx mcp-remote`) for Claude Desktop: not run here (the session may not execute external
      npm packages); documented as the standard bridge
- [x] 8. Docs: docs/wiki/mcp/ (setup for Claude Code, Claude Desktop, Cursor, generic clients; tool reference)
