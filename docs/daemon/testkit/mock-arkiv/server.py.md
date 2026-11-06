# daemon/testkit/mock-arkiv/server.py

## Purpose

Entry point of the mock Arkiv node. It serves JSON-RPC over HTTP and WebSocket, and a `/_mock/*` control API. Tests use it to create entities, mine blocks, reorganise the chain, and inject faults.

## Usage

```
python server.py --port 0 --seed 1      # prints READY <port>
```

Flags: `--host`, `--port` (0 picks a free port), `--seed`, `--max-log-range` (2000), `--auto-mine-ms`, `--chain-id` (0x76edaf), `--contract`, `--max-query-bytes`, `--max-depth`, `--max-conditions`, `--max-history`, `--dec-trim`.

## Key parts

- `Mock.call` dispatches the RPC methods. `arkiv_query` and `arkiv_getEntity` go to `query_lang.py`. Block and log methods read `state.py` and `logs.py`.
- `Mock.get_logs` enforces the log range cap, and returns `-32005` above it.
- `rpc_endpoint` and `ws_endpoint` serve the same methods. WebSocket adds `eth_subscribe` for `newHeads` and `logs`.
- The control API (`c_create`, `key_route`, `c_tx`, `c_mine`, `c_reorg`, `c_faults`, `c_state`, `c_reset`) changes state through `Chain.apply_ops`.
- `control_errors` maps `ControlError` to HTTP 400, and `Revert` to HTTP 409.

## Ownership and thread safety

Single event loop (aiohttp). No method awaits between reads and writes, so each operation is atomic. Not thread-safe, and not meant to be.

## Testing

`selftest.py` runs a subprocess of this server and checks the endpoints.

## Notes

- Faults never apply to `/_mock/*` endpoints.
- The control API accepts `owner` or `sender` for the transaction sender.
