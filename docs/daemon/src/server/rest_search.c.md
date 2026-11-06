# daemon/src/server/rest_search.c

## Purpose

Implements the REST API endpoints for vector search, pagination scroll, and entity counting in Arkivector (Phase D3). It connects query decoders, filter parsers, Roaring candidate evaluation, and flat index scanning to HTTP client requests.

## Architecture Role

Serves as the primary query execution interface within the HTTP server subsystem. Registered via `av_rest_register_search()`, it coordinates the filter engine (`filter/eval.h`), query planner (`query/planner.h`), and query executor (`query/executor.h`).

## Key Functions and Routes

| Handler / Function | Method & Route | Purpose |
| :--- | :--- | :--- |
| `av_rest_register_search` | Registration hook | Registers search, scroll, and count endpoints on the HTTP router. |
| `h_search` | `POST /v1/collections/{name}:search` | Performs vector similarity search. Parses query vector, evaluates filter expression, executes flat scan, and formats hits and optional explain plan. |
| `h_scroll` | `POST /v1/collections/{name}:scroll` | Cursor-based sequential iteration over entities matching an attribute filter, ordered by ascending entity key. Uses HMAC-signed pagination tokens. |
| `h_count` | `POST /v1/collections/{name}:count` | Calculates exact count of entities matching a filter expression by evaluating the candidate Roaring bitmap cardinality. |
| `h_search_batch` | `POST /v1/collections/{name}:batch` | Returns `501 not_implemented` during Phase D3 (deferred to Phase D4). |

## Memory Ownership

- **Decoded Vectors**: Query vectors decoded from JSON arrays, text strings, or base64 VectorBytes reside in a per-request arena or temporary buffer.
- **Filter AST**: Parsed filter trees are allocated within the request arena.
- **Candidate Bitmaps**: Intermediate Roaring bitmaps produced by `av_filter_eval()` are freed with `roaring_bitmap_free()`.
- **Search Results**: Structured hit results and JSON documents are allocated by `av_execute_search()` and released via `av_search_result_free()`.
- **Response Payloads**: Serialized JSON responses reside in `av_http_resp` buffers.

## Invariants

- **Vector Formats**: Accepts queries formatted as dense JSON arrays (`[0.1, 0.2]`), pgvector string literals (`"[0.1, 0.2]"`), sparse objects (`{"indices": [...], "values": [...]}`), or base64 VectorBytes.
- **Cosine Normalization**: If the collection uses a cosine distance opclass, the query vector is normalized in place prior to executing distance calculations.
- **Dimension Check**: The query vector's dimension must strictly match the collection definition.
- **Dual Filter Syntax**: Supports both structured JSON filters (`filter` object) and Arkiv `where` SQL-like strings (`where`). Both parse to an identical AST representation.
- **Scroll Cursors**: Pagination cursors are base64-encoded tokens formatted as `v1|as_of|last_key|filter_hash`, authenticated with an HMAC-SHA256 signature using a daemon-instance secret to prevent forgery.
- **Headers**: Responses include `Arkivector-Api-Version`, `Arkivector-As-Of-Block`, and `X-Request-Id`.

## Error Handling

- Dimension mismatch or invalid vector syntax returns `400 invalid_vector`.
- Syntax or semantic errors in filter expressions return `400 invalid_filter`.
- Corrupted, expired, or tampered scroll cursors return `400 invalid_cursor`.
- Missing collection returns `404 collection_not_found`.
- Unknown JSON properties return `400 bad_request`.
- Consistency timeout on `min_block` returns `504 consistency_timeout`.

## Thread Safety

- Handlers run across the query thread pool.
- Reads against segments, postings indexes, and LMDB stores operate without write locking, relying on collection staged publish snapshots and LMDB MVCC readers.
