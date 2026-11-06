# daemon/src/server/rest_collections.c

## Purpose

Implements the REST API endpoints for collection lifecycle management in Arkivector (Phase D3). Provides route handlers for listing, inspecting, creating, deleting, and following collections over HTTP.

## Architecture Role

Forms the collection administration interface within the HTTP server subsystem. Registered with the server's router via `av_rest_register_collections()`, connecting incoming REST calls to the collection registry (`av_registry`), persistent LMDB storage (`av_store`), and Arkiv RPC client (`av_rpc`).

## Key Functions and Routes

| Handler / Function | Method & Route | Purpose |
| :--- | :--- | :--- |
| `av_rest_register_collections` | Registration hook | Registers collection endpoints with the HTTP router. |
| `h_collections_list` | `GET /v1/collections` | Lists all registered collections, including schema metadata, vector dimensions, opclass, and live sync status. |
| `h_collection_get` | `GET /v1/collections/{name}` | Returns detailed definition, column type, opclass, index configuration, and block height of a specific collection. |
| `h_collection_create` | `POST /v1/collections` | Creates and registers a new collection. Validates opclass compatibility, vector dimensions, index parameters (`ef_construction >= 2 * m`), and membership writers. |
| `h_collection_delete` | `DELETE /v1/collections/{name}` | Deregisters the collection, halts synchronization workers, and removes stored segment/LMDB records. |
| `h_collection_follow` | `POST /v1/collections:follow` | Follows an existing collection on the Arkiv chain by fetching its manifest entity via `arkiv_getEntity`, parsing manifest JSON, and registering the collection. |

## Memory Ownership

- **Request Parsing**: Incoming JSON bodies are parsed into `yyjson_doc` instances.
- **Response Serialization**: Responses are constructed using `yyjson_mut_doc` and serialized into `av_http_resp` output buffers, which are freed once sent across the libuv TCP socket.
- **Collection State**: Collection definitions created by handlers are permanently stored in the `av_registry` and LMDB `collections` database.

## Invariants

- **Collection Names**: Must adhere to `^[a-zA-Z0-9_-]{1,64}$`.
- **Validation Rules**:
  - Opclass must be valid for the declared vector type (e.g. `vector_l2_ops`, `halfvec_cosine_ops`).
  - Vector dimensions must respect type bounds (1..16000 for dense vectors).
  - Unknown fields in JSON payloads are rejected with `400 bad_request`.
- **Standard Headers**: All responses include `Arkivector-Api-Version`, `Arkivector-As-Of-Block`, and `X-Request-Id`.
- **Deferred Flags**: `publish_manifest=true` on creation returns `501 not_implemented` (until Phase D5).

## Error Handling

- Non-existent collections yield `404 not_found` (`collection_not_found`).
- Attempting to recreate an existing collection yields `409 conflict` (`collection_already_exists`).
- Schema validation failures yield `400 bad_request` (`invalid_schema` or `invalid_field`).
- Errors are mapped to RFC 9457 Problem Details JSON format via `av_problem_from_err()`.

## Thread Safety

- Handlers run across the thread pool of query workers.
- Shared registry lookups and collection additions/deletions are synchronized using the registry's internal mutex.
