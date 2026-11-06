# daemon/src/server/idempotency.c

## Purpose

Implements the HTTP request idempotency management engine declared in `idempotency.h`. It computes SHA-256 request signatures, interfaces with the LMDB `idem` table, coordinates concurrent in-flight requests using condition variables, stores final HTTP response bodies, and manages cache expiration.

## Role in Architecture

Sits between the HTTP request dispatcher (`http.c` / `router.c`) and write endpoints (`rest_entities.c`). Protects against duplicate writes, double-spend of gas, and inconsistent state updates during client network timeouts and retries.

## Key Types and Functions

| Function / Helper | Role |
| :--- | :--- |
| `compute_request_hash` | Computes SHA-256 over `method`, `path`, and raw HTTP `body` bytes. |
| `in_flight_node` | Node in a hash table tracking currently executing requests, holding a `pthread_cond_t` for waiting followers. |
| `av_idempotency_mgr_create` | Initializes manager, configures in-flight table, and ensures LMDB `idem` database is open. |
| `av_idempotency_check` | Looks up key in memory and LMDB. If completed, returns cached response. If in-flight, waits on condition variable. If absent, creates in-flight node and returns `AV_IDEM_NEW`. |
| `av_idempotency_complete` | Writes response payload into LMDB `idem` record with timestamp, notifies waiting threads via `pthread_cond_broadcast`, and removes the in-flight node. |
| `av_idempotency_abort` | Removes the in-flight node and wakes waiting threads with an error indicator if execution failed catastrophically. |
| `av_idempotency_record_free` | Frees `response_body` string. |
| `av_idempotency_mgr_destroy` | Cleans up in-flight entries, destroys mutexes/condition variables, and frees manager memory. |

## Memory Ownership

- **In-Flight Nodes**: In-memory nodes are allocated on the heap during active processing and freed upon `av_idempotency_complete` or `av_idempotency_abort`.
- **Cached Response**: Response strings fetched from LMDB are allocated with `malloc` and transferred to the caller.

## Invariants

- **Leader-Follower Synchronization**: Only the first thread registering a key executes the write path. All other threads block on `pthread_cond_wait()` until the leader calls `complete` or `abort`.
- **Atomic State Transitions**: An in-flight entry transitions to completed atomically within an LMDB transaction.
- **TTL Eviction**: Records older than 86,400 seconds are treated as non-existent and overwritten.

## Error Handling

- Hash mismatches against existing records return `AV_ERR` with `e->code = "idempotency_conflict"` (HTTP 409).
- Timeout waiting on an in-flight condition variable returns `AV_ERR` with `e->code = "deadline_exceeded"`.

## Thread Safety

- Thread-safe across multiple concurrent query worker threads.
- In-memory in-flight table access is synchronized via a `pthread_mutex_t`, and LMDB read/write transactions adhere to LMDB's single-writer concurrency model.
