# Errors

Every error is an RFC 9457 problem document (`application/problem+json`). Branch on `code`, never on `detail`.

```json
{
  "type": "urn:arkivector:error:dimension_mismatch",
  "title": "Dimension mismatch",
  "status": 422,
  "detail": "expected 4 dimensions, not 2",
  "instance": "/v1/collections/notes/search",
  "code": "dimension_mismatch",
  "retryable": false,
  "request_id": "01M78E2K5G7XQR7NJYZ7325RPD",
  "field": "/vector"
}
```

| Field | Meaning |
| :--- | :--- |
| `code` | Stable machine code (table below) |
| `status` | HTTP status |
| `retryable` | Whether the same request may succeed later |
| `detail` | Human explanation; may change between versions |
| `request_id` | Echo of `X-Request-Id`; quote it in bug reports |
| `field` | JSON pointer to the bad field (`invalid_argument`, `invalid_vector`, `dimension_mismatch`) |
| `position` | Byte offset in `where` (`invalid_filter`) |
| `vb_error` | VectorBytes decode error code (`VB_*`) |
| `as_of_block` | Current block (`consistency_timeout`, `not_ready`) |
| `revert_error` | Solidity error name, e.g. `NotOwner` (`tx_reverted`) |

## Codes

| Code | Status | Retry | When |
| :--- | :---: | :---: | :--- |
| `bad_request` | 400 | no | Malformed JSON, wrong JSON types, unknown body fields |
| `unauthorized` | 401 | no | API key missing or invalid |
| `forbidden` | 403 | no | Key lacks the scope; MCP Origin rejected |
| `write_disabled` | 403 | no | The daemon has no signer |
| `collection_not_found` | 404 | no | |
| `entity_not_found` | 404 | no | Not live at `as_of_block` |
| `route_not_found` | 404 / 405 | no | Unknown path, or method not allowed (`Allow` header) |
| `collection_exists` | 409 | no | |
| `not_owner` | 409 | no | The wallet doesn't own the entity |
| `tx_reverted` | 409 | no | The Arkiv transaction reverted |
| `idempotency_conflict` | 409 | no | `Idempotency-Key` reused with a different body |
| `payload_too_large` | 413 | no | Body > 32 MiB or entity bytes > 128 KiB |
| `unsupported_media_type` | 415 | no | Body is not `application/json` |
| `invalid_argument` | 422 | no | Out-of-range or inconsistent field |
| `invalid_vector` | 422 | no | Vector failed pgvector or VectorBytes validation |
| `dimension_mismatch` | 422 | no | Wrong number of dimensions |
| `invalid_filter` | 422 | no | Filter parse or type error |
| `rate_limited` | 429 | yes | Honour `Retry-After` |
| `internal` | 500 | no | A bug: report the `request_id` |
| `not_implemented` | 501 | no | Not in this build; check `/v1/version` capabilities |
| `arkiv_rpc_error` | 502 | yes | The Arkiv node failed |
| `not_ready` | 503 | yes | Collection still bootstrapping |
| `timeout` | 504 | yes | Request deadline exceeded inside the daemon |
| `consistency_timeout` | 504 | yes | `min_block` not reached in time |

## Retrying

Retry only when `retryable` is `true`, or on transport errors (connection refused, timeout before any response). Reads (search, batch, scroll, count, get, prepare) are safe to retry with exponential backoff. Writes are safe to retry only with an `Idempotency-Key`, which the SDKs send by default.
