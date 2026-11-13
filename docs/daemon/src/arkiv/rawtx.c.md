# daemon/src/arkiv/rawtx.c

## Purpose

Decodes a signed EIP-1559 (type 2) transaction and recovers its sender. Inputs are untrusted: client-signed transactions from `entities:submit` and answers from a remote signer.

## Role in Architecture

Used by `write/writer.c` (`av_writer_submit`, checks before broadcast) and `arkiv/tx.c` (remote signer verification). Introduced for GitHub issue #12 / CCR-0001. Fuzz target: `test/fuzz/fuzz_rawtx.c`.

## Key Functions

| Function | Role |
| :--- | :--- |
| `rlp_next` | Reads one RLP item inside a bound. Rejects truncated items, non-canonical length forms and a single byte below 0x80 that is wrapped as a string. |
| `item_u64`, `item_u256` | Canonical unsigned integers (no leading zero byte), bounded to 8 or 32 bytes. |
| `av_rawtx_decode` | Checks the type byte (legacy and other types are rejected with a clear message), exactly 12 fields and no trailing bytes, `to` of 0 or 20 bytes, a list access list, yParity 0/1, non-zero r and s, low s (EIP-2), tip ≤ max fee. Computes the sighash over the contiguous fields 0–8, recovers the public key with `secp256k1_ecdsa_recover` (static context), derives the sender, and hashes the raw bytes for the transaction hash. |
| `av_rawtx_decode_hex` | Hex wrapper with a size cap (`AV_RAWTX_MAX_BYTES`, 512 KiB). |

## Memory Ownership

`av_rawtx.data` points into the caller's buffer. One temporary heap buffer holds the sighash preimage and is freed before return.

## Error Handling

All decode failures return `invalid_argument` with a short reason prefixed `signed transaction:`.

## Thread Safety

Stateless and reentrant.
