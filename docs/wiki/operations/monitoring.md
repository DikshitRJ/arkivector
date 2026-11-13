# Monitoring

## Endpoints

| Endpoint | Use |
| :--- | :--- |
| `GET /healthz` | Liveness probe |
| `GET /readyz` | Readiness: `200` when every collection is live |
| `GET /v1/sync` | Per-collection state, `as_of_block`, `head_block`, `lag_blocks`, `last_error` |
| `GET /metrics` | Prometheus metrics |

## Prometheus

```yaml
scrape_configs:
  - job_name: arkivector
    static_configs:
      - targets: ["127.0.0.1:8080"]
```

| Metric | Type | Meaning |
| :--- | :--- | :--- |
| `arkivector_http_requests_total` | counter | Requests, by route and status |
| `arkivector_http_request_duration_seconds` | histogram | Request latency, by route |
| `arkivector_http_in_flight_requests` | gauge | Requests being handled |
| `arkivector_http_connections` | gauge | Open connections |
| `arkivector_kernel_tier` | gauge | Selected distance kernel tier (label) |

Useful alerts: readiness false for more than a few minutes, `lag_blocks` growing in `/v1/sync`, a rising rate of 5xx responses, p99 search latency.

## Logs

`log.format = "json"` writes one JSON object per line for log collectors; `text` is the default. `log.level = "debug"` shows sync batches, plan decisions and RPC calls. Secrets (private key, access key, API keys) are never logged.

Notable messages:

| Message | Meaning |
| :--- | :--- |
| `bootstrap done at block N (M live)` | A collection finished its snapshot |
| `hnsw graph full at N vectors` | New vectors go to the exact tail until the next rebuild |
| `hnsw graph rebuilt over N live vectors in X ms` | Graph rebuilt after many updates or a full graph |
| `reorg` / `reconcile divergence` | Chain reorganisation handled / local state differed from the chain (should be rare) |
