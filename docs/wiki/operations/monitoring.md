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
| `arkivector_hnsw_graph_loads_total` / `_graph_load_failures_total` | counter | HNSW band files loaded at start / rejected and rebuilt, by collection |
| `arkivector_hnsw_rebuilds_total` / `_rebuilds_in_progress` | counter / gauge | Background HNSW builds adopted / running |
| `arkivector_hnsw_build_ms` | histogram | Background build time |
| `arkivector_hnsw_bands` / `_bands_dropped_total` | gauge / counter | Expiry bands / expired bands released |
| `arkivector_hnsw_graph_saves_total` | counter | HNSW band files written |
| `arkivector_write_transactions_total` / `_tx_replacements_total` | counter | Write transactions sent / stuck ones resent with bumped fees |
| `arkivector_write_gas_used_total` / `_gas_reserved_total` | counter | Gas used vs gas limit reserved by mined writes |
| `arkivector_write_effective_gas_price_wei` | gauge | `effectiveGasPrice` of the last mined write |
| `arkivector_write_fee_quotes_total` | counter | Fee quotes by `source` (`fee_history`, `gas_price`, `static`) |
| `arkivector_write_gas_estimate_fallback_total` | counter | Writes whose gas limit fell back to the static estimate |

Useful alerts: readiness false for more than a few minutes, `lag_blocks` growing in `/v1/sync`, a rising rate of 5xx responses, p99 search latency.

## Logs

`log.format = "json"` writes one JSON object per line for log collectors; `text` is the default. `log.level = "debug"` shows sync batches, plan decisions and RPC calls. Secrets (private key, access key, API keys) are never logged.

Notable messages:

| Message | Meaning |
| :--- | :--- |
| `bootstrap done at block N (M live)` | A collection finished its snapshot |
| `graph full at N vectors` | New vectors go to the exact tail until the next rebuild |
| `band B graph rebuilt in the background over N vectors in X ms` | Background rebuild adopted (after many updates, a full graph, or a missing file) |
| `loaded <file> (N vectors, saved at block B)` | A saved graph was reused at start |
| `graph file <file> rejected (...)` | A corrupt or mismatched graph file; the band is rebuilt in the background |
| `expiry band B expired at block N` | A band whose entities all expired was released |
| `gas estimate fallback (...)` | `eth_estimateGas` failed; the static gas limit was used |
| `tx ... not mined after N blocks: replaced by ...` | A stuck transaction was resent with bumped fees |
| `reorg` / `reconcile divergence` | Chain reorganisation handled / local state differed from the chain (should be rare) |
