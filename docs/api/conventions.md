# Conventions

## 1. Transport

- **Protocol:** HTTP/1.1 (keep-alive). TLS is terminated by a reverse proxy, never by the daemon.
- **Default base URL:** `http://127.0.0.1:8080`. SDKs read `ARKIVECTOR_URL` when no URL is given, and fall back to this default.
- **Content type:** requests with a body MUST send `Content-Type: application/json` (UTF-8). Any other type gets `415 unsupported_media_type`. Success responses are `application/json`. Errors are `application/problem+json` ([errors.md](errors.md)). `/metrics` is `text/plain`.
- **Compression:** the daemon accepts and produces `gzip` when the client sends `Content-Encoding: gzip` / `Accept-Encoding: gzip`. SDKs MAY use it. It is not required.
- **Body limit:** 32 MiB per request. Larger requests get `413 payload_too_large`.

## 2. Versioning

- The URL prefix `/v1` is the **major** API version. Breaking changes require `/v2`.
- `spec/VERSION` (semver) is the contract version. `GET /v1/version` returns it as `api_version`.
- **Compatibility rule for SDKs:** an SDK built for contract `1.x.y` MUST work with any daemon whose `api_version` major is `1`. It MUST NOT fail on unknown response fields (forward compatibility). It MUST use capabilities, not version comparison, to decide whether a feature exists.
- **Compatibility rule for the daemon:** it MUST ignore unknown **query parameters**. It MUST reject unknown **body fields** with `400 bad_request` (strict request schemas, `additionalProperties: false`), so SDK typos fail loudly.
- Every response carries `Arkivector-Api-Version: <api_version>`.

## 3. Authentication

- Optional. When the daemon is started with an `api_keys_file`, every `/v1/*`, `/qdrant/*`, and `/mcp` request MUST carry `Authorization: Bearer <key>`.
- Unauthenticated endpoints, always open: `/healthz`, `/readyz`, `/v1/version`, `/metrics`.
- Scopes: `read` (GET, search, scroll, count), `write` (entity writes, plus read), `admin` (collection create/follow/delete, plus write).
- Missing or invalid key → `401 unauthorized`. Insufficient scope → `403 forbidden`.
- SDKs read `ARKIVECTOR_API_KEY` when no key is given. They MUST NOT log the key.

## 4. Request and response headers

| Header | Direction | Rule |
| :--- | :--- | :--- |
| `X-Request-Id` | both | Client MAY send (≤ 64 chars, `[A-Za-z0-9._-]`). Daemon echoes it, or generates a ULID. Always present on responses and repeated as `request_id` in problems. |
| `Arkivector-As-Of-Block` | response | Present on every data response (collections, entities, search, scroll, count). Equals the body's `as_of_block`. |
| `Arkivector-Api-Version` | response | Contract version (e.g. `1.1.0`). |
| `Idempotency-Key` | request | Optional on write endpoints ([entities.md §6](entities.md#6-idempotency)). |
| `Retry-After` | response | Seconds. Present on `429`, `503`. |
| `User-Agent` | request | SDKs MUST send `arkivector-<lang>/<sdk-version>` (e.g. `arkivector-python/0.1.0`). |

## 5. JSON conventions

| Item | Rule |
| :--- | :--- |
| Field names | `snake_case` on the wire. SDKs MAY expose idiomatic names (camelCase in TS) but MUST convert. |
| Absent vs null | Optional request fields: omit them. Do not send `null` (it gets `400`), unless the schema allows null. Responses use `null` where the schema says `[T, "null"]`. |
| Entity keys | `0x` + 64 **lowercase** hex. The daemon accepts any case in requests and always responds in lowercase. |
| Addresses | `0x` + 40 hex. Responses use EIP-55 checksum case. Requests accept any case. |
| Block numbers | JSON integers (safe below 2⁵³). |
| Large integers (`u64`, `u256`) | Decimal **strings** inside `TypedValue`. |
| Decimals (`dec`) | Decimal strings with ≤ 18 fraction digits, e.g. `"4.5"`. Responses trim trailing zeros (`"4.500"` → `"4.5"`). Never JSON floats. |
| Bytes | Base64 (RFC 4648 §4, with padding) in JSON (`vector_bytes`, `bytes` attributes). |
| Floats (vectors, distances) | JSON numbers. Vectors in requests are interpreted as float32. NaN/Infinity are invalid JSON and rejected. |
| Timestamps | None on the wire. Time is expressed in blocks (2 s each on Rabdi). |

## 6. Names

| Name | Pattern |
| :--- | :--- |
| Collection name | `^[a-z][a-z0-9._-]{0,62}$` |
| Attribute name | `^[a-zA-Z][a-zA-Z0-9._-]{0,31}$`, must not contain `--`, must not start with `av.` (reserved) or equal `arkiv.schema`, and must not be an Arkiv keyword (`AND OR NOT TRUE FALSE BETWEEN STARTSWITH EXISTS TYPEOF ORDER BY ASC DESC` or a type name) |
| External id (`id`) | 1–128 UTF-8 bytes. In URL paths, percent-encode it (RFC 3986 path segment). An `id` that matches `^0x[0-9a-fA-F]{64}$` is interpreted as a key, so ids MUST NOT have that shape. |

## 7. Limits (enforced by the daemon, mirrored by SDK pre-validation)

| Limit | Value | Error |
| :--- | ---: | :--- |
| Entities per upsert / prepare / delete / extend | 1,000 | `invalid_argument` |
| Transactions per submit | 256 | `invalid_argument` |
| `k` | 1–1,000 | `invalid_argument` |
| `offset + k` | ≤ 1,000 | `invalid_argument` |
| Searches per batch | 64 | `invalid_argument` |
| Scroll `limit` | 1–1,000 | `invalid_argument` |
| `where` length | 8,192 bytes | `invalid_filter` |
| Filter depth / leaves | 16 / 256 | `invalid_filter` |
| `ef_search` | 1–1,000 | `invalid_argument` |
| vector / halfvec dims | 1–16,000 | `invalid_vector` |
| bit dims | 1–64,000 | `invalid_vector` |
| sparsevec dim / nnz | 1–10⁹ / ≤ 16,000 | `invalid_vector` |
| User attributes per entity | 29 | `invalid_argument` |
| `str` attribute value | 128 UTF-8 bytes | `invalid_argument` |
| Bytes budget per entity (`av.vec` + `av.doc` + user `bytes`) | 131,072 bytes | `payload_too_large` |
| TTL | ≤ 7,884,000 blocks | `invalid_argument` |
| `consistency.timeout_ms` | ≤ 60,000 | `invalid_argument` |

## 8. Timeouts and retries (SDK obligations)

- Default client timeout: **30 s** for writes with `wait=receipt`/`indexed`, **10 s** otherwise. Configurable.
- **Retry only** when the problem has `retryable: true`, or on transport errors (connection refused/reset, timeout before any response byte).
- **Idempotent operations** (all GETs, search, search:batch, scroll, count, prepare) MAY be retried automatically: up to 3 attempts, exponential backoff 200 ms × 2ⁿ with ±20% jitter, honouring `Retry-After` when present.
- **Write operations** (upsert, delete, extend, submit) MUST NOT be auto-retried **unless** the SDK sent an `Idempotency-Key`. SDKs SHOULD generate one (UUIDv4) per write call by default, which makes write retries safe.
