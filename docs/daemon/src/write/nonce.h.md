# daemon/src/write/nonce.h

## Purpose

Declares the local EVM transaction nonce tracking, reservation, and resynchronization interface for the Arkivector write rail (Phase D5).

## Role in Architecture

Maintains an in-memory sequential nonce tracker for the daemon's signing account. This enables concurrent multi-transaction pipelining without waiting for previous transactions to be mined into blocks, while providing automatic recovery if nonces drift or transactions are rejected.

## Key Types and Functions

### Data Structures

```c
/* Opaque handle for nonce tracking */
typedef struct av_nonce_tracker av_nonce_tracker;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_nonce_tracker_create` | Initializes a nonce tracker for a given account address, seeding its counter from `eth_getTransactionCount(addr, "pending")`. |
| `av_nonce_acquire` | Atomically claims the next sequential transaction nonce for a pending broadcast. |
| `av_nonce_release` | Reclaims an unsubmitted nonce on broadcast failure to prevent nonce gaps. |
| `av_nonce_resync` | Queries the RPC node to resynchronize the local counter with on-chain pending state. |
| `av_nonce_tracker_destroy` | Destroys the tracker and releases synchronization resources. |

### Function Signatures

```c
av_status av_nonce_tracker_create(av_rpc *rpc, const uint8_t address[20],
                                  av_nonce_tracker **out_nt, av_err *e);

av_status av_nonce_acquire(av_nonce_tracker *nt, uint64_t *out_nonce, av_err *e);

void av_nonce_release(av_nonce_tracker *nt, uint64_t nonce);

av_status av_nonce_resync(av_nonce_tracker *nt, av_err *e);

void av_nonce_tracker_destroy(av_nonce_tracker *nt);
```

## Memory Ownership

- **Tracker Handle**: Allocated on the heap via `malloc`. The caller owns `nt` and must destroy it using `av_nonce_tracker_destroy()`.
- **RPC Handle**: The tracker borrows the `av_rpc *` handle for network queries; `rpc` must outlive the tracker.

## Invariants

- **Monotonically Increasing Nonces**: Successive calls to `av_nonce_acquire` yield strictly increasing integers under normal execution.
- **Gap Avoidance**: If an acquired nonce is not broadcast (e.g. ABI encoding failure), `av_nonce_release` allows it to be re-used or triggers a resync before subsequent transactions proceed.
- **Account Binding**: A nonce tracker instance is strictly bound to a single 20-byte EVM address.

## Error Handling

- RPC failures during initial count query or resync return `AV_ERR` with `code = "upstream_error"`.
- Transaction rejections with RPC errors indicating `"nonce too low"` or `"replacement transaction underpriced"` trigger an automatic `av_nonce_resync()`.

## Thread Safety

- Completely thread-safe.
- All state modifications (acquiring, releasing, resyncing) are guarded by an internal `pthread_mutex_t`.
