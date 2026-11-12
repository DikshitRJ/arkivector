## ADDED Requirements

### Requirement: Tools mirror the REST API
Each MCP tool SHALL run the corresponding /v1 endpoint and return its response as `structuredContent`,
or its problem object as text with `isError: true`.

#### Scenario: Search
- **WHEN** an agent calls arkivector_search with a vector, a `where` filter and include_document
- **THEN** the result holds the same hits, distances and documents as POST /v1/collections/{name}/search

### Requirement: Scopes per tool
With API-key auth enabled, each tool call SHALL require the scope of its /v1 route.

#### Scenario: Read key
- **WHEN** a read-only key calls arkivector_upsert
- **THEN** the tool result is an error with code `forbidden` and nothing is written

### Requirement: Streamable HTTP transport
The server SHALL answer `ping`, accept notifications with 202, reject JSON-RPC batches, answer GET
with 405, and reject non-loopback Origin headers unless API-key auth is enabled.

#### Scenario: DNS rebinding
- **WHEN** a request with `Origin: https://evil.example` reaches a daemon without API keys
- **THEN** it is rejected with 403
