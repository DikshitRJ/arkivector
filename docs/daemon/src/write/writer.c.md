# daemon/src/write/writer.c

## Purpose

Implements the managed writer pipeline declared in `writer.h`. It drives entity input validation, diffs attributes to determine create versus patch operations, coordinates batch packing with `batcher.c`, handles nonce acquisition, invokes `signer.c` and `tx.c`, broadcasts transactions via `rpc.c`, maps receipt logs to keys, executes local write-through updates, and handles consistency waits.

## Role in Architecture

Orchestrates the entire Phase D5 write stack. Coordinates interaction between `storage/lmdb_store.c`, `core/collection.c`, `arkiv/abi.c`, `arkiv/tx.c`, `arkiv/signer.c`, `write/batcher.c`, and `write/nonce.c`.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `resolve_existing_entity` | Checks `store/lmdb_store.h` for existing `av.id` mappings, reads stored entity row, and verifies ownership. |
| `diff_attributes` | Compares proposed attributes against stored attributes to construct differential mutations for `PATCH` operations, adding tombstones (type 0) for deleted fields. |
| `map_ttl_to_expiry` | Converts relative TTL objects (blocks, seconds, days) or absolute block numbers into `(expiresAt, minLifetime)` tuple. |
| `decode_revert_reason` | Simulates failed transactions via `eth_call` to extract the 4-byte custom error selector and look up the name in the Arkiv 22-error catalogue. |
| `poll_receipts` | Periodically queries `eth_getTransactionReceipt` via `av_rpc` with exponential backoff until all transactions are mined or deadline is reached. |
| `apply_write_through` | Commits written entities directly into LMDB and collection postings under the collection's publish lock at receipt block height. |
| `av_writer_upsert` | Core entrypoint for managed entity writes. |
| `av_writer_prepare` | Entrypoint for client-side signing, returning unsigned transactions (nonce, gas limit), predicted keys and a fee quote (`av_fee_quote_rpc`). |
| `av_writer_submit` | Client-signed transactions (`entities:submit`): decodes each with `rawtx.c` and checks chain id, Arkiv contract, value 0, `execute` selector and that the sender is a writer, before any broadcast; relays them in order (`already known` counts as accepted); or takes hashes a wallet broadcast. Polls receipts (no replacement), collects `EntityCreated` keys, and waits for the collection with `wait=indexed`. Invalidates the writer's nonces if a submitted transaction came from the daemon's own address. |

## Memory Ownership

- **Scratch Arenas**: Allocates ephemeral memory for ABI buffers, attribute sorting arrays, and RLP encodings from a temporary thread-local arena reset at function completion.
- **Result Structures**: Allocates output result items and transaction hashes with `calloc`; ownership is transferred to the caller.

## Invariants

- **Attribute Diffing**: Identical vector hashes and attribute byte payloads produce zero calldata bytes, avoiding unnecessary gas expenditure.
- **Log Index Correlation**: In multi-operation batches, created entity keys are extracted from transaction receipt `EntityCreated` logs by strictly matching log event indices to create operation offsets.
- **Monotonicity**: Write-through indexing updates the collection only if `receipt_block >= collection->as_of_block`.

## Error Handling

- Reverted transactions populate `e->code = "tx_reverted"` and embed the contract error name (e.g. `InvalidValueType`, `MaxAttributesExceeded`).
- Failures during partial batch execution report the successfully mined transactions alongside the revert reason for the failed batch.
- RPC timeouts during broadcasting or receipt polling trigger retry backoff per `ARCHITECTURE.md §9.9`.

## Thread Safety

- Fully thread-safe.
- Multiple HTTP request threads can execute `av_writer_upsert` concurrently; transaction nonces are allocated atomically and write transactions are synchronized at the LMDB transaction boundary.
