# Proposal: daemon-issues-11-13-14-15

## Why
Four roadmap items from issue #19 ("What's next after v1.1.0"):
- **#11** HNSW graphs lived only in memory: rebuilt from scratch at every start, rebuilt on the sync
  thread after many updates (stalling ingestion), and expired rows stayed as tombstones.
- **#13** Write responses predicted entity keys from a per-request counter starting at 0, so keys were
  wrong whenever the wallet had created entities elsewhere.
- **#14** Fees were fixed (20 gwei / 1.5 gwei) and the gas fallback reserved far more than a create
  uses (about 135k gas on Rabdi); stuck transactions were never replaced.
- **#15** The Qdrant layer's upsert and delete returned success without writing and search payloads
  were always null: silent data loss for Qdrant clients.

## What changes
- `index/hnsw_index.c`: per-expiry-band graphs (`[hnsw] band_blocks`), a worker thread for builds and
  checkpoints, adopt-and-swap on the applier, band files under `data_dir/segments/<collection>/` with a
  CRC'd header, a fully validating loader (`av_segment_hnsw_restore`) and verification against the flat
  segment. Expired bands are released whole; `live_entities` excludes expired rows.
- Writer: entityNonce via `eth_call`, local in-flight floor, one submit lock for key + nonce + send,
  keys from `EntityCreated` receipt logs; `write/fees.c` fee chain (feeHistory → gasPrice → static,
  capped), configurable gas margin, logged fallback, stuck-transaction replacement, nonce resync on
  failure, metrics. New `[write]` config section.
- Qdrant: `server/qdrant_map.c` maps points/filters/payloads per docs/api/qdrant.md; upsert and delete
  use the managed writer; read-only daemons return 403.

## Contract impact
None. Response shapes are unchanged (keys stay in `results[].key`; gas observability is in metrics
and logs, not in the write response), and the Qdrant behaviour follows the frozen docs/api/qdrant.md.
No capability is added or removed (`qdrant` already existed).

## Out of scope
Rabdi end-to-end measurements (keys vs EntityCreated logs on chain, reserved vs used gas within 25%):
they need a funded testnet wallet and are left for the next Rabdi run. Qdrant retrieve/scroll (501),
removing attributes missing from a Qdrant re-upsert.
