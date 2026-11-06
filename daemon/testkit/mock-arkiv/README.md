# mock-arkiv

Deterministic simulator of an Arkiv node for daemon tests. Python 3.11+, `aiohttp`, `pycryptodome` (Keccak).

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python server.py --port 0 --seed 1        # prints: READY <port>
.venv/bin/python selftest.py                        # starts its own server, runs the checks
```

Flags: `--port P` (0 = free), `--seed N`, `--max-log-range 2000`, `--auto-mine-ms 0`, `--chain-id 0x76edaf`,
`--contract 0x4400...0044`, `--max-query-bytes 16384`, `--max-depth 64`, `--max-conditions 1024`,
`--max-history 256` (blocks of reorg history), `--dec-trim` (emit `dec` as `4.5` instead of 18 decimals), `--host`.

Everything is deterministic for a given seed and sequence of calls: block hash = `sha256(parent || n || epoch)`,
timestamps `1_700_000_000 + 2n`, entity keys per `entity-key-derivation.md`, tx hashes from block hash + index + calldata.

## JSON-RPC (POST `/` or `/<accessKey>`, any key accepted; WebSocket on the same paths)

`arkiv_query`, `arkiv_getEntity`, `arkiv_getBlockTiming`, `eth_chainId`, `eth_blockNumber`, `eth_getBlockByNumber`,
`eth_getBlockByHash`, `eth_getLogs` (range cap -> `-32005`), `eth_getTransactionByHash` (input = `execute(ops)`
calldata, selector `0x49650044`), `eth_getTransactionReceipt`, `eth_getTransactionCount`, `eth_call`
(`entityNonce(address)`), `eth_estimateGas` (size based), `eth_gasPrice`, `net_version`, `web3_clientVersion`,
`eth_syncing`, `eth_sendRawTransaction` (returns `-32601 "not implemented in mock v0"`, phase D5).
WS only: `eth_subscribe` (`newHeads`, `logs` with `address`/`topics` filter) and `eth_unsubscribe`; a notification is pushed
per mined block (and `removed:true` logs for blocks rolled back by a reorg). Batches are supported.

Errors: `-32001` parse, `-32002` type, `-32003` literal, `-32004` limits (query bytes, nesting depth, condition count),
`-32602` invalid params, `-32005` log range exceeded, `-32601` unknown method, HTTP 429 (fault).
Liveness: an entity is visible at head `B` iff `expiresAt > B`. Reads see only sealed blocks.

## Control API (`/_mock/*`, JSON, never affected by faults)

Mutations go into the **pending block** (head+1); they become visible when mined.
Attribute objects: `{"name","type","value"[,"indexed"]}` with `value` in wire encoding (`u64`/`u256` as int or hex string,
`dec` as decimal string, `addr`/`key`/`bytes` as 0x-hex). `indexed` defaults to true (false for `bytes`; true is rejected).
In `patch`, `{"name":"x","type":"tombstone"}` removes an attribute. Any call takes an optional `owner` (the tx sender);
for key-targeted ops it defaults to the entity's current owner, for creates to `0x1111...1111`.
A failed op reverts the whole tx with HTTP 409 `{"error":"NotOwner","args":{...}}`; invalid input is HTTP 400.

```bash
M=http://127.0.0.1:$PORT/_mock
# create (key derived from keccak(chainId||contract||owner||nonce||salt)); returns {key, block, txHash, logs}
curl -s -XPOST $M/entities -d '{"owner":"0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045","min_lifetime":100,
  "flags":0,"salt":0,"attributes":[{"name":"arkiv.schema","type":"str","value":"my.app"},{"name":"score","type":"dec","value":"4.5"}]}'
# create with preset key/creator/absolute expiry (seed data; skips the 7,884,000-block max-lifetime check)
curl -s -XPOST $M/entities -d '{"key":"0x30..","owner":"0x11..","creator":"0x11..","expires_at":10000000,"attributes":[]}'
# bulk create: body is an array (one tx each) -> {"created":[...]}
curl -s -XPOST $M/entities -d '[{"min_lifetime":50},{"min_lifetime":60}]'
curl -s -XPATCH  $M/entities/$KEY -d '{"attributes":[{"name":"score","type":"dec","value":"1"}]}'
curl -s -XPOST   $M/entities/$KEY/extend   -d '{"min_lifetime":500}'      # or {"expires_at":N}
curl -s -XPOST   $M/entities/$KEY/transfer -d '{"new_owner":"0x2222222222222222222222222222222222222222"}'
curl -s -XDELETE $M/entities/$KEY
# multi-op tx (one execute() batch, atomic): ops are objects with "op": create|patch|extend|transfer|delete
curl -s -XPOST $M/tx -d '{"owner":"0x11..","ops":[{"op":"create","min_lifetime":9},{"op":"delete","key":"0x.."}]}'
curl -s -XPOST $M/mine -d '{"n":5}'                    # seal n blocks; pending txs go into the first
curl -s -XPOST $M/auto_mine -d '{"interval_ms":2000}'  # 0 stops
curl -s -XPOST $M/reorg -d '{"depth":3,"extra_blocks":0,"replacement_ops":[{"op":"create","owner":"0x11..","min_lifetime":9}]}'
curl -s -XPOST $M/faults -d '{"drop_ws_ratio":0.2,"http_429_ratio":0.1,"latency_ms":50,"fail_methods":["arkiv_query"]}'
curl -s $M/faults ; curl -s "$M/state?keys=1" ; curl -s -XPOST $M/reset -d '{"seed":2}'
```

* **Reorg**: rewinds `depth` blocks to the retained snapshot, discards pending txs, bumps the epoch (block hashes change),
  then mines `depth + extra_blocks` blocks; `replacement_ops` (txs `{owner?, ops:[..]}` or bare ops) land in the first one.
  If a replacement op reverts, the chain is left untouched and the call fails.
* **Faults** (seeded RNG): `http_429_ratio` (429 + `Retry-After`), `drop_ws_ratio` (closes the socket instead of
  sending/answering), `latency_ms` (before each RPC), `fail_methods` (list, or `{method: ratio}`; `"*"` = all)
  returns `-32603`. Partial updates merge; send zeros/`[]` to clear. `reset` clears faults unless `keep_faults`.
* `GET /_mock/state[?all=1][&keys=1]` returns head, pending, counts, faults and the live entities.

## Source map

`server.py` HTTP/WS/control, `state.py` chain + `apply_ops(sender, ops)` (the single mutation entry used by the control
API and by D5 tx decoding), `query_lang.py` lexer/parser/evaluator, `logs.py` events/ABI/filters, `faults.py`.

## Doc ambiguities and choices

* Contract address: the research docs use `0x4400...0044` (events/keys) and elsewhere `0x4444...4444` (examples). Default is
  `0x4400...0044`; override with `--contract`. Key derivation reproduces the documented example hash.
* Predicates only see **indexed** attributes; unindexed ones never match (`EXISTS`, `TYPEOF`, comparisons) but can be sorted on.
* `dec` is emitted with 18 fractional digits (table in `arkiv_query.md` 4.2); `--dec-trim` gives the short form.
* Default order without `ORDER BY` is creation order; ties under `ORDER BY` fall back to `$key` ascending.
* `extend` bumps `updatedAt`; `transfer` does not. Expired entities stay in memory but are invisible; ops on them revert `EntityExpired`.
* Nesting-depth limit counts AND/OR chains as left-associative binary trees (a 65-term chain is rejected).
