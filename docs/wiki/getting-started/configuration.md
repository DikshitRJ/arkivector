# Configuration

The daemon reads an optional TOML file (`arkivectord --config <file>`). Every key can be overridden with an environment variable named `ARKIVECTOR_<SECTION>_<KEY>`, and the environment wins over the file. [`daemon/arkivector.example.toml`](https://github.com/DikshitRJ/arkivector/blob/main/daemon/arkivector.example.toml) is a commented starting point.

```toml
[server]
listen = "127.0.0.1:8080"     # keep on localhost unless api_keys_file is set
# api_keys_file = "/etc/arkivector/api_keys.toml"
# ratelimit_rps = 50
# ratelimit_burst = 100

[arkiv]
rpc_urls = ["https://rpc.rabdi.db-chain.testnet.arkiv.network"]
ws_url = "wss://rpc.rabdi.db-chain.testnet.arkiv.network"   # "" = HTTP polling
access_key = ""               # Arkiv Hub access key
chain_id = 7794095            # Rabdi
confirmations = 2             # stay this many blocks behind head (reorg safety)
max_log_range = 2000          # eth_getLogs block range per request

[storage]
data_dir = "/var/lib/arkivector"
map_size = 1073741824         # LMDB map size in bytes

[sync]
poll_interval_ms = 2000

[write]                       # managed writes (need ARKIV_PRIVATE_KEY)
# max_fee_cap_gwei = 50       # never pay more than this per gas
# gas_margin_pct = 20         # margin over eth_estimateGas

[hnsw]
# persist = 1                 # save graphs under data_dir/segments and load them at start
# band_blocks = 1296000       # expiry band width (30 days of 2 s blocks); 0 = one band

[log]
level = "info"                # debug | info | warn | error
format = "text"               # text | json
```

## Reference

Defaults are what the daemon uses when a key is absent (the example file sets some of them differently).

| Key (`ARKIVECTOR_<SECTION>_<KEY>`) | Default | Meaning |
| :--- | :--- | :--- |
| `ARKIV_PRIVATE_KEY` (plain env var) | unset | Wallet private key that signs writes (hex, `0x` optional). Unset = read-only. Never put it in the file |
| `server.listen` | `127.0.0.1:8080` (`0.0.0.0:8080` in Docker) | HTTP listen address |
| `server.threads` | `0` (= 4) | Query worker threads |
| `server.max_body` | 32 MiB | Request body limit |
| `server.max_connections` | 1024 | Open connections |
| `server.request_timeout_ms` / `idle_timeout_ms` | 30000 / 60000 | Receive a request within / keep-alive idle |
| `server.api_keys_file` | empty | Enables [API-key auth](../operations/security.md) |
| `server.ratelimit_rps` / `ratelimit_burst` | 0 (off) | Per-client token-bucket rate limit |
| `arkiv.rpc_urls` | Rabdi RPC | JSON-RPC endpoints (comma-separated in the env var), tried in order |
| `arkiv.ws_url` | Rabdi WebSocket | New-block notifications; `""` = HTTP polling |
| `arkiv.access_key` | empty | Arkiv Hub access key (sent as `X-API-KEY`, never logged) |
| `arkiv.chain_id` | `7794095` | Chain id used when signing |
| `arkiv.confirmations` | `0` | Blocks behind head before data is applied; `2` is a good value on Rabdi |
| `arkiv.max_log_range` | `2000` | Blocks per `eth_getLogs` request during catch-up |
| `arkiv.rpc_budget_per_sec` | `20` | RPC rate budget (token bucket) |
| `arkiv.timeout_ms` | `10000` | Per-RPC timeout |
| `storage.data_dir` | `/var/lib/arkivector` (`/data` in Docker) | Local index and state |
| `storage.map_size` | 64 GiB | LMDB map size (address space, allocated as used) |
| `sync.poll_interval_ms` | `1000` | Polling interval when WebSocket is off or down |
| `sync.bootstrap_parallelism` | `4` | Parallel key ranges during the initial snapshot |
| `sync.reconcile_interval_s` | `60` | How often the local count is checked against the chain |
| `sync.ring_size` | `128` | Recent block hashes kept for reorg detection |
| `write.max_fee_cap_gwei` | `0` (no cap) | Upper bound for `maxFeePerGas` |
| `write.priority_fee_percentile` | `50` | `eth_feeHistory` reward percentile used as the tip |
| `write.fee_history_blocks` | `20` | Blocks of fee history |
| `write.max_fee_gwei` / `priority_fee_gwei` | `20` / `1.5` | Static fees when the node has neither `eth_feeHistory` nor `eth_gasPrice` |
| `write.gas_margin_pct` | `20` | Margin added to `eth_estimateGas` |
| `write.fallback_gas_base` | `60000` | Gas when estimation fails: this plus 150,000 per operation |
| `write.replace_after_blocks` | `10` | Resend a transaction not mined after this many blocks, with fees raised by at least 12.5% |
| `write.max_replacements` | `3` | Resends per transaction (`0` = never) |
| `hnsw.persist` | `1` | Save HNSW graphs under `<data_dir>/segments/` and load them at start (`0` = rebuild in the background at every start) |
| `hnsw.band_blocks` | `1296000` | Expiry band width in blocks; a band is released when all its entities have expired. `0` = one band per collection |
| `log.level` | `info` | `debug`, `info`, `warn`, `error` |
| `log.format` | `text` | `text` or `json` |

For example `storage.map_size` is `ARKIVECTOR_STORAGE_MAP_SIZE`. Client-side variables read by the SDKs: `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`) and `ARKIVECTOR_API_KEY`.

## API keys file

```toml
[[keys]]
id = "my-app"
key = "a-long-random-secret"     # or: sha256 = "<hex sha256 of the secret>"
scopes = ["read", "write"]       # read | write | admin
```

Clients send `Authorization: Bearer <secret>`. Scopes: `read` (GET, search, scroll, count), `write` (entity writes, plus read), `admin` (create, follow and delete collections, plus write). See [Security](../operations/security.md).

## Command line

| Flag | Meaning |
| :--- | :--- |
| `--config <file>` | TOML configuration file |
| `--data-dir <dir>` | Overrides `storage.data_dir` |
| `--listen <host:port>` | Overrides `server.listen` |
| `--resync` | Discard stored sync state and bootstrap every collection again |
| `--version` | Print the version and exit |
| `--licenses` | Print the licences of the copied pgvector and PostgreSQL code |
| `--debug-routes` | Enable `/_debug/...` routes (testing only) |
