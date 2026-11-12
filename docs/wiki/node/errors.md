# Errors

All API errors extend `ApiError`, which carries the daemon's [problem document](../api/errors.md):

```ts
import { ApiError, NotFoundError } from "@dikshitrj/arkivector";

try {
  await client.get("notes", "nope");
} catch (e) {
  if (e instanceof NotFoundError) {
    console.log(e.code, e.status, e.retryable, e.requestId); // entity_not_found 404 false 01M7…
  }
}
```

Fields: `code`, `status`, `title`, `detail`, `retryable`, `requestId`, `field`, `position`, `vbError`, `asOfBlock`, `revertError`, and `raw` (the whole problem object).

| Class | Codes |
| :--- | :--- |
| `TransportError` | No HTTP response (connection refused, timeout) |
| `AuthError` | `unauthorized`, `forbidden`, `write_disabled` |
| `NotFoundError` | `collection_not_found`, `entity_not_found`, `route_not_found` |
| `ConflictError` | `collection_exists`, `not_owner`, `tx_reverted`, `idempotency_conflict` |
| `ValidationError` | `bad_request`, `invalid_argument`, `invalid_vector`, `invalid_filter`, `dimension_mismatch`, `payload_too_large`, `unsupported_media_type` |
| `UnavailableError` | `rate_limited`, `arkiv_rpc_error`, `not_ready`, `timeout`, `consistency_timeout` |
| `NotImplementedByDaemonError` | The daemon lacks the capability (thrown before sending) |
| `ApiError` | `internal` and unknown codes |
| `VectorBytesError` | Local encode/decode errors (`VB_*`) |
