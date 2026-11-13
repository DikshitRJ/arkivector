# Consistency

## `as_of_block`

The daemon applies the chain block by block and publishes each block atomically. Every data response reflects exactly one block, `as_of_block` (also in the `Arkivector-As-Of-Block` header). All results of one response, and all searches of one batch, come from the same block.

An entity is live at block B when it exists, is not deleted, and `expires_at_block > B`: the same rule Arkiv uses, so results match what `arkiv_query` returns at B.

## Read your writes: `min_block`

A write returns `block_number`. Send it as `consistency.min_block` and the daemon waits (without busy-spinning) until it has applied that block:

```bash
curl -s $AV/v1/collections/notes/search -H 'content-type: application/json' \
  -d '{"vector": [1, 0, 0, 0], "k": 3, "consistency": {"min_block": 15, "timeout_ms": 4000}}'
```

For `GET …/entities/{ref}`, pass `?min_block=15&timeout_ms=4000`.

If `timeout_ms` (≤ 60,000) passes first, the answer is `504 consistency_timeout` with the current `as_of_block`; it is safe to retry.

Shortcuts:

- write with `"wait": "indexed"`: the write returns only once the daemon has indexed it;
- SDK session consistency (`Client(session_consistency=True)`, `new Arkivector({sessionConsistency: true})`): the client remembers its last write block and sends it automatically.

## Sync states

| State | Meaning | Reads |
| :--- | :--- | :--- |
| `init` | Registered, sync not started | `503 not_ready` |
| `bootstrap` | Initial snapshot running | `503 not_ready`, unless `min_block` is given (then it waits) |
| `catch_up` | Replaying blocks, lag above the threshold | Allowed, results are older |
| `live` | Lag ≤ 2 blocks | Allowed |
| `reorg_recovery` | Re-fetching entities touched by a reorg | Allowed, from the last consistent block |
| `degraded` | Arkiv RPC unreachable | Allowed, from the last published block (stale) |

`sync_state` in search responses tells you which applies; non-`live` states are not errors. Per-collection detail is at [`GET /v1/sync`](health.md#get-v1sync).
