# Operational Endpoints

## `GET /healthz`
Always `200 {"status":"ok"}` while the process can serve HTTP. No auth.

## `GET /readyz`
`200 {"ready": true, "collections": {"demo.docs": "live"}}` when **every** collection is `live`. Otherwise `503` with the same shape and `Retry-After`. No auth. Load balancers gate on it.

## `GET /v1/version`
No auth. SDKs call it **once per client** (lazily, on first use) and cache the result.

```json
{
  "daemon_version": "0.1.0",
  "api_version": "1.1.0",
  "pgvector_version": "0.8.7",
  "kernel_tier": "x86-64-v3",
  "chain_id": 7794095,
  "writes_enabled": true,
  "signer": { "kind": "keystore", "address": "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266" },
  "capabilities": ["ops.version", "collections.read", "search.exact", "…"]
}
```

| Field | Meaning |
| :--- | :--- |
| `api_version` | Contract version implemented. SDKs check that the major equals their own and fail with a clear error otherwise |
| `kernel_tier` | `x86-64`, `x86-64-v3`, `x86-64-v4`, `aarch64`, `native` |
| `writes_enabled` | True iff a managed signer is configured. Client-side signing (`entities:prepare` + `entities:submit`) works either way |
| `signer` | *1.1.* `null`, or `{kind, address}` with `kind` one of `env` (key in an environment variable), `keystore` (encrypted Web3 Secret Storage file), `remote` (Web3Signer / Clef; the daemon never holds the key). Never key material |
| `capabilities` | Subset of [`spec/capabilities.json`](../../spec/capabilities.json) |

**Capability rule:** before calling an optional feature, an SDK checks the capability. If it is missing, the SDK raises its *not implemented* error **locally**, without sending the request. If an SDK skips the check and the daemon answers `501 not_implemented`, it raises the same error.

## `GET /v1/sync`
`{"collections": [SyncStatus, …]}` (scope read).

## `GET /metrics`
Prometheus text format. No auth. Metric names are listed in `ARCHITECTURE.md §16.4`. They are **not** part of the SDK contract.
