# Consistency and Sync State

## 1. As-of-block semantics

The daemon applies Arkiv changes **block by block** and publishes each block atomically. Every data response reflects exactly one block, `as_of_block`, returned in the body and in the `Arkivector-As-Of-Block` header. All results in one response, and all searches in one batch, come from the same block.

`as_of_block` is the last block **fully applied** locally. It is ≤ the chain head. An entity is live at block B iff it exists, is not deleted, and `expires_at_block > B`. That is the same rule Arkiv uses, so results match what `arkiv_query` would return at B.

## 2. Read-your-writes: `consistency.min_block`

```json
"consistency": { "min_block": 582160, "timeout_ms": 4000 }
```

- If `as_of_block ≥ min_block`, the request runs immediately.
- Otherwise the daemon **waits** (without busy-spinning) until it publishes `min_block`, then runs the request.
- If `timeout_ms` elapses first → `504 consistency_timeout`, with `as_of_block` = the current block. This is retryable.

Typical flows:
- Managed write with `wait: "receipt"` → use the returned `block_number` as `min_block` on the next read. Or use `wait: "indexed"` and skip `min_block`.
- Direct or prepared write by the client → use its own receipt `blockNumber`.

SDKs SHOULD offer a "session consistency" option that remembers the highest `block_number` seen from its own writes and sends it automatically as `min_block`.

For `GET …/entities/{ref}`, the same fields are passed as query parameters `min_block` and `timeout_ms`.

## 3. Sync states (`SyncState`)

| State | Meaning | Reads allowed? |
| :--- | :--- | :--- |
| `init` | Collection registered, sync not started | No → `503 not_ready` |
| `bootstrap` | Initial snapshot scan running | No → `503 not_ready` (unless `min_block` is given, then it waits) |
| `catch_up` | Replaying logs, lag > threshold | Yes (results are older) |
| `live` | Lag ≤ threshold (default 2 blocks) | Yes |
| `reorg_recovery` | Re-fetching keys touched by a reorg | Yes (the last consistent block is served) |
| `degraded` | Arkiv RPC unreachable | Yes, from the last published block. Results are stale |

`SyncStatus = { name, state, as_of_block, head_block, lag_blocks, last_error }`, returned in `Collection.sync` and `GET /v1/sync`.

`SearchResponse.sync_state` lets callers decide whether stale data is acceptable. SDKs MUST expose it and MUST NOT treat non-`live` states as errors.
