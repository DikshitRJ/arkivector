# daemon/src/server/rest_entities.c

## Purpose

Implements the REST API endpoints for single entity retrieval in Arkivector (Phase D3). Provides point lookups by 32-byte hex key or string ID, projection filtering, and minimum block consistency gating.

## Architecture Role

Provides the entity point-read endpoint within the HTTP server subsystem. Registered via `av_rest_register_entities()`, bridging HTTP requests to the collection's LMDB entity tables (`entities:<coll>` and `ids:<coll>`) and the collection synchronization condition variable for read consistency.

## Key Functions and Routes

| Handler / Function | Method & Route | Purpose |
| :--- | :--- | :--- |
| `av_rest_register_entities` | Registration hook | Registers entity endpoints with the HTTP router. |
| `h_entity_get` | `GET /v1/collections/{name}/entities/{ref}` | Retrieves a single entity by 32-byte hex key (`0x...`) or string ID (`av.id`). Supports projection flags (`include`), consistency gating (`min_block`), and timeout (`timeout_ms`). |
| Entity mutation placeholders | `POST/PUT/DELETE …/entities` | Return `501 not_implemented` during Phase D3 (write path is implemented in Phase D5). |

## Memory Ownership

- **Request Parsing**: Path and query parameters (`include`, `min_block`, `timeout_ms`) are parsed directly from request strings.
- **Entity Hydration**: Unpacked entity attributes and row metadata are populated into an arena or directly into an ephemeral `yyjson_mut_doc`.
- **Response**: Serialized JSON payload is stored in the `av_http_resp` buffer for transmission.

## Invariants

- **Key Resolution**: If `{ref}` begins with `0x` followed by 64 hex characters, it is parsed directly as a 32-byte entity key. Otherwise, it is resolved via the collection's `ids:<coll>` index mapping string `av.id` to its 32-byte key.
- **Consistency Gating (`min_block`)**:
  - When `min_block` is specified, the handler waits on `av_collection_wait_block` until `collection->as_of_block >= min_block` or `timeout_ms` expires.
  - If the timeout expires before reaching `min_block`, returns `504 gateway_timeout` with code `consistency_timeout`.
- **Bootstrap Readiness**: If the collection is synchronizing in `bootstrap` mode and no explicit `min_block` is requested, returns `503 not_ready` with a `Retry-After: 1` header.
- **Standard Headers**: Every response includes `Arkivector-Api-Version`, `Arkivector-As-Of-Block`, and `X-Request-Id`.

## Error Handling

- Missing collection returns `404 not_found` (`collection_not_found`).
- Missing entity returns `404 not_found` (`entity_not_found`).
- Timeout on `min_block` returns `504 gateway_timeout` (`consistency_timeout`).
- Malformed reference or invalid query parameters return `400 bad_request`.

## Thread Safety

- Handlers execute concurrently on worker threads in the query pool.
- Reads are executed against thread-safe LMDB read transactions.
- Waiting on block updates uses pthread condition variables associated with collection read locks.
