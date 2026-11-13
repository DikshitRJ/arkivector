# daemon/src/arkiv/tx.c

## Purpose

Implements EIP-1559 transaction serialization and hashing routines declared in `tx.h`. It constructs the payload list for RLP encoding, appends the EIP-2718 `0x02` type envelope prefix, computes the Keccak-256 sighash, and produces wire-ready signed raw transactions.

## Role in Architecture

Part of the Phase D5 write stack. Coordinates with `rlp.c` to produce canonical Ethereum wire encodings, with `signer.c` to obtain valid ECDSA signatures, and provides transaction data to `rpc.c` for broadcast.

For a remote signer (GitHub issue #12), `av_tx_sign_and_encode` does not compute a signature. It sends the transaction object (`from`, `to`, `gas`, fees, `nonce`, `chainId`, `type`, `data`) with `eth_signTransaction` or `account_signTransaction`, accepts the raw transaction as a string or as `{raw, tx}`, decodes it with `rawtx.c`, and accepts it only if `av_tx_matches` is true and the recovered sender is the configured address. Otherwise it returns `write_disabled` and nothing is broadcast.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `encode_tx_fields` | Populates common EIP-1559 fields (`chainId`, `nonce`, `maxPriorityFeePerGas`, `maxFeePerGas`, `gasLimit`, `to`, `value`, `data`, `accessList`) into an `av_rlp` builder. |
| `av_tx_sighash` | Encodes fields into an RLP list, prefixes with `0x02`, and hashes the combined payload via `av_keccak256`. |
| `av_tx_encode_signed` | Encodes fields plus signature parameters `(yParity, r, s)` into an RLP list, prefixes with `0x02`, and copies the result into an arena. |
| `av_tx_compute_fees` | Clamps dynamic fee estimates to avoid mempool eviction while respecting operator-configured max gas price ceilings. |

## Memory Ownership

- **RLP Builder Memory**: Uses an ephemeral `av_rlp` struct whose buffer is initialized and freed locally.
- **Arena Memory**: The final wire payload returned by `av_tx_encode_signed` is copied into the caller's `av_arena`.

## Invariants

- **Field Order**: Enforces canonical EIP-1559 list ordering:
  1. `chainId` (`uint64`)
  2. `nonce` (`uint64`)
  3. `maxPriorityFeePerGas` (`uint64`)
  4. `maxFeePerGas` (`uint64`)
  5. `gasLimit` (`uint64`)
  6. `to` (20 bytes)
  7. `value` (`uint64`, 0)
  8. `data` (bytes)
  9. `accessList` (empty list `[]`)
  10. *(If signed)* `yParity` (`uint8`), `r` (32 bytes), `s` (32 bytes)
- **Access List**: The access list field is always encoded as an empty RLP list `0xc0`.

## Error Handling

- Memory allocation errors during RLP buffer growth return `AV_ERR` and set `code = "internal"`.
- Passing NULL transaction pointers or invalid signature dimensions sets `code = "invalid_argument"`.

## Thread Safety

- Thread-safe and reentrant.
- Relies solely on stack-allocated data structures and thread-isolated `av_arena` buffers.
