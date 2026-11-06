# daemon/src/write/nonce.c

## Purpose

Implements the nonce management and recovery routines declared in `nonce.h`. It maintains the local transaction counter, manages in-flight nonce allocation, handles error reclamation, and queries `eth_getTransactionCount` to realign local counters with the mempool.

## Role in Architecture

Ensures ordered transaction submission across multiple concurrent worker threads invoking `writer.c`, eliminating nonce collisions and transaction stalls.

## Key Types and Functions

| Function / Type | Role |
| :--- | :--- |
| `struct av_nonce_tracker` | Holds target account address, current expected nonce, highest allocated nonce, pending gap mask, mutex, and RPC handle. |
| `fetch_chain_nonce` | Issues `eth_getTransactionCount(addr, "pending")` via `rpc.c` and parses the returned hex quantity. |
| `av_nonce_tracker_create` | Allocates tracker struct, initializes mutex, and seeds counter via `fetch_chain_nonce`. |
| `av_nonce_acquire` | Under mutex lock, increments and returns the next contiguous nonce. |
| `av_nonce_release` | Records released nonce or rolls back counter if the released nonce matches the highest issued. |
| `av_nonce_resync` | Refetches pending nonce from chain and resets counter if chain reports a higher or lower pending value. |
| `av_nonce_tracker_destroy` | Destroys mutex and frees the tracker struct. |

## Memory Ownership

- **Tracker State**: Dynamically allocated using `calloc` in `av_nonce_tracker_create`, freed in `av_nonce_tracker_destroy`.
- **Zero Static State**: All variables are encapsulated in `av_nonce_tracker`.

## Invariants

- **Pending State Targeting**: Nonce queries always target the `"pending"` block tag, ensuring local counts reflect unmined transactions accepted into the node's mempool.
- **Mutex Protection**: State inspection and mutation are mutually exclusive.

## Error Handling

- Nonce parse errors or RPC transport drops set `code = "upstream_error"` and leave the previous local counter intact until retry.
- In-flight nonce discrepancies caught upon broadcast trigger an asynchronous resync.

## Thread Safety

- Thread-safe. Internal mutex guarantees thread synchronization across all API functions.
