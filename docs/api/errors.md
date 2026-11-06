# Errors

## 1. Problem format (RFC 9457)

Every non-2xx response (except `204`) has `Content-Type: application/problem+json`:

```json
{
  "type": "urn:arkivector:error:invalid_filter",
  "title": "Invalid filter",
  "status": 422,
  "detail": "unknown operator 'contains' at /filter/and/1",
  "code": "invalid_filter",
  "retryable": false,
  "request_id": "01JC8Z5Q2W3E4R5T6Y7U8I9O0P",
  "position": 17
}
```

| Field | Always? | Meaning |
| :--- | :---: | :--- |
| `type` | ✅ | `urn:arkivector:error:<code>` |
| `title` | ✅ | Short human summary (stable per code) |
| `status` | ✅ | HTTP status |
| `code` | ✅ | Machine code from [`spec/error-codes.json`](../../spec/error-codes.json). **SDKs branch on this only.** |
| `retryable` | ✅ | Whether the identical request may succeed later |
| `detail` | | Human detail. Not stable, never parse it |
| `request_id` | ✅ | Echo of `X-Request-Id` |
| `field` | | JSON pointer into the request body (`invalid_argument`, `invalid_vector`, `dimension_mismatch`) |
| `position` | | Byte offset in `where` (`invalid_filter`) |
| `vb_error` | | `VB_*` code (`invalid_vector` from `vector_bytes`) |
| `as_of_block` | | Current block (`consistency_timeout`, `not_ready`) |
| `revert_error` | | Solidity custom error name, e.g. `NotOwner` (`tx_reverted`) |

## 2. Code registry

| code | status | retryable | When |
| :--- | :---: | :---: | :--- |
| `bad_request` | 400 | no | Malformed JSON, wrong JSON types, unknown body fields |
| `unauthorized` | 401 | no | Key missing or invalid |
| `forbidden` | 403 | no | Scope insufficient |
| `write_disabled` | 403 | no | No signer configured |
| `collection_not_found` | 404 | no | |
| `entity_not_found` | 404 | no | Not live at `as_of_block` |
| `route_not_found` | 404 | no | Unknown path or method |
| `collection_exists` | 409 | no | |
| `not_owner` | 409 | no | Signer doesn't own the target entity |
| `tx_reverted` | 409 | no | Arkiv tx reverted (`revert_error`) |
| `idempotency_conflict` | 409 | no | Key reused with a different body |
| `payload_too_large` | 413 | no | Body > 32 MiB, or entity bytes > 128 KiB |
| `unsupported_media_type` | 415 | no | |
| `invalid_argument` | 422 | no | Out-of-range or inconsistent field (`field`) |
| `invalid_vector` | 422 | no | pgvector/VectorBytes validation (`vb_error`) |
| `dimension_mismatch` | 422 | no | |
| `invalid_filter` | 422 | no | Parse or type error (`position`) |
| `rate_limited` | 429 | yes | `Retry-After` |
| `internal` | 500 | no | Bug. Report the `request_id` |
| `not_implemented` | 501 | no | Feature not in this build (check capabilities) |
| `arkiv_rpc_error` | 502 | yes | Upstream RPC failure |
| `not_ready` | 503 | yes | Bootstrapping or degraded. `Retry-After` |
| `timeout` | 504 | yes | Request deadline exceeded inside the daemon |
| `consistency_timeout` | 504 | yes | `min_block` not reached |

New codes may be added in minor versions. **SDKs MUST map unknown codes to a generic API error** that keeps `code`, `status`, and `retryable`.

## 3. Required SDK error model

Every SDK exposes one base error type carrying: `code`, `status`, `title`, `detail`, `retryable`, `request_id`, and the raw problem object (for `field`, `position`, `vb_error`, `as_of_block`, `revert_error`). It also exposes:

| Category | Codes | Python | TypeScript | Rust (`ErrorKind`) | C (`av_status_t`) | C++ |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Transport | (no HTTP response) | `TransportError` | `TransportError` | `Transport` | `AV_E_TRANSPORT` | `TransportError` |
| Auth | unauthorized, forbidden, write_disabled | `AuthError` | `AuthError` | `Auth` | `AV_E_AUTH` | `AuthError` |
| Not found | *_not_found | `NotFoundError` | `NotFoundError` | `NotFound` | `AV_E_NOT_FOUND` | `NotFoundError` |
| Conflict | collection_exists, not_owner, tx_reverted, idempotency_conflict | `ConflictError` | `ConflictError` | `Conflict` | `AV_E_CONFLICT` | `ConflictError` |
| Validation | bad_request, invalid_*, dimension_mismatch, payload_too_large, unsupported_media_type | `ValidationError` | `ValidationError` | `Validation` | `AV_E_VALIDATION` | `ValidationError` |
| Unavailable | rate_limited, arkiv_rpc_error, not_ready, timeout, consistency_timeout | `UnavailableError` | `UnavailableError` | `Unavailable` | `AV_E_UNAVAILABLE` | `UnavailableError` |
| Not implemented | not_implemented | `NotImplementedByDaemon` | `NotImplementedByDaemonError` | `NotImplemented` | `AV_E_NOT_IMPLEMENTED` | `NotImplementedError` |
| Other | internal, unknown | `ApiError` | `ApiError` | `Api` | `AV_E_API` | `ApiError` |
| Local codec | VB_* raised by the SDK's own encoder/decoder | `VectorBytesError(code)` | `VectorBytesError` | `VectorBytes(code)` | `AV_E_VECTORBYTES` | `VectorBytesError` |

All HTTP-derived categories are subclasses (or variants) of the base API error.
