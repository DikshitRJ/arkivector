# Health and version

Operational endpoints. `/healthz`, `/readyz`, `/v1/version` and `/metrics` never need an API key.

## `GET /healthz`

Liveness: `200` while the process serves HTTP.

```bash
curl -s $AV/healthz
```

```json
{"status": "ok"}
```

## `GET /readyz`

`200` when every collection is `live`, otherwise `503` with the same shape and a `Retry-After` header. Point load balancers here.

```bash
curl -s $AV/readyz
```

```json
{"ready": true, "collections": {"demo.docs": "live", "notes": "live"}}
```

## `GET /v1/version`

What this daemon is and can do. The SDKs call it once and cache it.

```bash
curl -s $AV/v1/version
```

```json
{
  "daemon_version": "1.1.0",
  "api_version": "1.1.0",
  "pgvector_version": "0.8.7",
  "kernel_tier": "x86-64-v4",
  "chain_id": 7794095,
  "writes_enabled": true,
  "signer": { "kind": "keystore", "address": "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266" },
  "capabilities": ["ops.version", "ops.sync", "ops.metrics", "collections.read", "collections.write",
                   "collections.follow", "entities.read", "search.exact", "search.hnsw", "search.filter.json",
                   "search.filter.where", "search.batch", "search.explain", "scroll", "count",
                   "consistency.min_block", "collections.manifest", "entities.write", "entities.prepare", "entities.submit",
                   "entities.verify", "idempotency", "auth.api_keys", "qdrant", "mcp"]
}
```

| Field | Meaning |
| :--- | :--- |
| `api_version` | REST contract version. Clients check that the major version matches |
| `kernel_tier` | Distance kernels picked for this CPU: `x86-64`, `x86-64-v3` (AVX2/FMA), `x86-64-v4` (AVX-512), `aarch64` |
| `writes_enabled` | `true` when the daemon has its own signer (managed writes). Writes signed by your wallet work either way |
| `signer` | `null`, or the signer's `kind` (`env`, `keystore`, `remote`) and public `address`. Never the key. API 1.1 |
| `capabilities` | Features this build supports |

## `GET /v1/sync`

Per-collection sync state.

```bash
curl -s $AV/v1/sync
```

```json
{"collections": [
  {"name": "notes", "state": "live", "as_of_block": 15, "head_block": 16, "lag_blocks": 1, "last_error": null}
]}
```

States: `init`, `bootstrap`, `catch_up`, `live`, `reorg_recovery`, `degraded`. See [Consistency](consistency.md#sync-states).

## `GET /metrics`

Prometheus text format. See [Monitoring](../operations/monitoring.md).
