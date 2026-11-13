# Errors

Every API error is an `ApiError` subclass carrying the daemon's [problem document](../api/errors.md):

```python
from arkivector import ApiError, NotFoundError

try:
    client.get("notes", "nope")
except NotFoundError as e:
    print(e.code, e.status, e.retryable, e.request_id)   # entity_not_found 404 False 01M7…
    print(e.problem)                                      # raw problem dict (field, position, ...)
```

| Exception | Codes |
| :--- | :--- |
| `TransportError` | No HTTP response (connection refused, timeout) |
| `AuthError` | `unauthorized`, `forbidden`, `write_disabled` |
| `NotFoundError` | `collection_not_found`, `entity_not_found`, `route_not_found` |
| `ConflictError` | `collection_exists`, `not_owner`, `tx_reverted`, `idempotency_conflict` |
| `ValidationError` | `bad_request`, `invalid_argument`, `invalid_vector`, `invalid_filter`, `dimension_mismatch`, `payload_too_large`, `unsupported_media_type` |
| `UnavailableError` | `rate_limited`, `arkiv_rpc_error`, `not_ready`, `timeout`, `consistency_timeout` |
| `NotImplementedByDaemon` | The daemon lacks the capability (raised before sending) |
| `ApiError` | `internal` and unknown codes |
| `VectorBytesError` | Local encode/decode errors (`VB_*` codes) |

Retryable errors are already retried by the client up to `max_retries` times ([Client](client.md#retries-and-timeouts)).
