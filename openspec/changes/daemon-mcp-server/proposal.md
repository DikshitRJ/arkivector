# Proposal: daemon-mcp-server

## Why
`POST /mcp` answered the MCP handshake, but it fell short of docs/api/mcp.md and the MCP spec (2025-06-18):
- Tools re-implemented the /v1 logic badly. Search returned only key/id/score (no distance,
  attributes or documents) and ignored `filter` and `include_document`. Get returned only a key and a
  block, without checking liveness. Count had no `filter`.
- No `structuredContent`, and no `arkivector_upsert` even with a signer.
- `ping` was "Method not found", notifications got 204 instead of 202, and an unknown tool was an
  isError result instead of JSON-RPC -32602.
- No Origin validation (the spec requires it against DNS rebinding).
- API-key scopes were never checked per tool, so `/mcp` only needed the read scope.

## What changes
- Every tool runs the matching /v1 handler through the router, so semantics, validation and error
  codes are exactly the REST API's, as docs/api/mcp.md requires. Success → `structuredContent` = the
  /v1 response, plus a text block. Errors → `isError` with the problem object as text.
- Each inner call is checked against the caller's key with the scope of its own /v1 route.
- `arkivector_upsert` is listed when the daemon has a signer (default `wait: "indexed"`, so the
  agent can search what it just wrote). Tool titles, descriptions per argument, annotations
  (readOnlyHint, ...), and server `instructions`.
- Transport: `ping`, 202 for notifications and responses, version negotiation (2025-06-18,
  2025-03-26, 2024-11-05), batches rejected (removed in 2025-06-18), GET/DELETE → 405 Allow: POST
  (no SSE stream), Origin check (loopback always, other origins only with API-key auth).
- HTTP parser keeps the `Origin` header; the REST layer gets the server's auth context.

## Out of scope
`text` queries (need an embedder hook; the tool explains how to pass `vector` instead), SSE streams,
sessions, resources and prompts.
