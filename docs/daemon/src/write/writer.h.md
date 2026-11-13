# daemon/src/write/writer.h

## Purpose

Declares the managed entity writer interface for Arkivector (Phase D5). It coordinates the full write lifecycle: entity validation, upsert-by-id differential mutation analysis, operation batching, transaction construction, cryptographic signing, mempool broadcast, receipt tracking, revert error decoding, write-through indexing, and consistency waiting.

## Role in Architecture

Acts as the master coordinator for state changes initiated through the daemon's REST API (`POST /v1/collections/{name}/entities`, `:delete`, `:extend`). It translates high-level entity payloads into atomic on-chain transactions executed against Arkiv's canonical contract `0x4400000000000000000000000000000000000044`.

## Key Types and Functions

### Data Structures

```c
typedef enum {
    AV_WAIT_NONE,     /* 202 Accepted: broadcast and return predicted keys */
    AV_WAIT_RECEIPT,  /* 200 OK: wait for EVM transaction receipts */
    AV_WAIT_INDEXED   /* 200 OK: wait for daemon's local index to publish receipt block */
} av_wait_mode;

typedef enum {
    AV_OP_RES_CREATE,
    AV_OP_RES_PATCH,
    AV_OP_RES_NOOP,
    AV_OP_RES_DELETE,
    AV_OP_RES_EXTEND
} av_op_outcome;

typedef struct {
    char id[129];
    uint8_t key[32];
    av_op_outcome outcome;
} av_writer_item_result;

typedef struct {
    av_writer_item_result *items;
    size_t n_items;
    char (*tx_hashes)[67];      /* Array of "0x..." 64-hex transaction hashes */
    size_t n_tx_hashes;
    uint64_t block_number;      /* Highest receipt block number (or 0 if wait=none) */
} av_writer_result;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_writer_upsert` | Executes managed upsert-by-id for a batch of entities. |
| `av_writer_delete` | Executes entity deletions for a list of entity keys or external IDs. |
| `av_writer_extend` | Extends the expiration block height for a list of entities. |
| `av_writer_prepare` | Prepares unsigned `execute(ops)` transactions and predicted keys for client-side signing without submitting to chain. `av_prepare_result` also carries the fee suggestion (`max_fee_per_gas`, `max_priority_fee_per_gas`, `fee_source`). |
| `av_writer_submit` | Validates and broadcasts client-signed transactions, or waits for wallet-broadcast hashes; returns `av_submit_result` (tx hashes, highest receipt block, created keys). No signer needed. |
| `av_writer_result_free` | Releases heap memory allocated within an `av_writer_result` struct. |

### Function Signatures

```c
av_status av_writer_upsert(av_collection *c, av_store *store, av_signer *signer,
                           av_rpc *rpc, av_nonce_tracker *nonce,
                           const av_entity_input *entities, size_t n_entities,
                           av_wait_mode wait, uint32_t timeout_ms,
                           av_writer_result *out, av_err *e);

av_status av_writer_prepare(av_collection *c, av_store *store,
                            const uint8_t sender[20], av_rpc *rpc,
                            const av_entity_input *entities, size_t n_entities,
                            av_arena *a, av_prepared_batch *out, av_err *e);

void av_writer_result_free(av_writer_result *r);
```

## Memory Ownership

- **Result Buffers**: `av_writer_upsert` allocates the `items` and `tx_hashes` arrays on the heap. The caller owns `out` and must invoke `av_writer_result_free(out)` when finished.
- **Prepared Calldata**: `av_writer_prepare` allocates raw calldata buffers and transaction arrays from the caller-supplied `av_arena *a`.
- **Borrowed Handles**: Collection, store, signer, and RPC client handles are borrowed by the writer for the duration of the call.

## Invariants

- **Upsert-by-ID Semantics**:
  - If `av.id` does not exist in the collection: an entity is created via `AV_OP_CREATE` using a 128-bit CSPRNG salt.
  - If `av.id` exists and is owned by `signer`: attribute changes produce an `AV_OP_PATCH`; if attributes and vectors are identical, the item is marked `AV_OP_RES_NOOP` and omitted from calldata.
  - If `av.id` exists but is owned by a different address: execution halts and fails immediately with `409 not_owner`.
- **Atomic Batches**: Operations grouped into an individual EVM transaction execute atomically.
- **Write-Through Indexing**: Upon receiving a successful transaction receipt, the writer immediately updates local LMDB rows and index segments at the receipt block, ensuring read-your-writes consistency before the sync engine processes the block.

## Error Handling

- Unauthorized mutations (not owner) set `code = "not_owner"` and HTTP status 409.
- Transaction reverts decode the contract's revert error string (e.g. `SortedAttributesRequired`, `NotOwner`, `EntityExpired`) from the 22-error catalogue and set `code = "tx_reverted"`.
- Timeout during receipt or indexed wait sets `code = "deadline_exceeded"`.

## Thread Safety

- Thread-safe. Multiple worker threads can invoke writer functions concurrently. Nonce allocation is serialized safely via `av_nonce_tracker`, and write transactions against LMDB are serialized by LMDB's single-writer architecture.
