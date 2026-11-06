# daemon/testkit/mock-arkiv/state.py

## Purpose

Holds the chain and entity state of the mock. It applies operations to the pending block, seals blocks, and reorganises the chain.

## Key types

| Type | Meaning |
| :--- | :--- |
| `Attr` | One attribute: name, type, indexed flag, value. |
| `Entity` | Immutable entity. Replaced on every change, so snapshots are cheap. |
| `Op` | One operation: create, patch, extend, transfer or delete. |
| `Tx`, `Block` | Transactions and blocks. |
| `Snapshot` | Immutable state at the end of a block. |
| `Chain` | Blocks, the pending block, the working map, snapshots and history. |
| `RpcError`, `ControlError`, `Revert` | Error types for RPC, control input and EVM reverts. |

## Key functions

| Function | Purpose |
| :--- | :--- |
| `parse_op`, `parse_attr` | Validate control-API input. Limits follow `limits-and-quotas.md`. |
| `Chain.apply_ops(sender, ops)` | Apply one transaction to the pending block. It is atomic: a revert restores the previous state. |
| `Chain.seal_block()` | Make the pending block the head. Save the snapshot. Remove old history. |
| `Chain.reorg(depth, replacement, extra)` | Rewind, change the epoch, mine replacement blocks. Restore the chain on failure. |
| `Chain.eth_call` | Answer `entityNonce(address)` only. |
| `derive_key` | Entity key `keccak256(chainId || contract || owner || nonce || salt)`. |

## Ownership and thread safety

Single-threaded. Each public method runs without an `await`.

## Testing

`selftest.py`. Golden hashes in `logs.py` are checked at import time.

## Notes

- Block hashes come from the parent hash, the number and the reorg epoch. Timestamps are `GENESIS + 2n`.
- `Entity.expires_at` must be greater than the head for an entity to be live.
