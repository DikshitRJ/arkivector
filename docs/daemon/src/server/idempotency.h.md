# daemon/src/server/idempotency.h

## Purpose

Declares the HTTP write idempotency interface for Arkivector (Phase D5). It guarantees exactly-once execution semantics for mutating REST requests (`POST /v1/collections/{name}/entities`, `:delete`, `:extend`) supplied with an `Idempotency-Key` header.

## Role in Architecture

Operates as a middleware layer in `server/http.c` and `server/rest_entities.c`. It caches transaction results in LMDB database `idem`, prevents duplicate on-chain transaction submissions from network retries, and coordinates in-flight request deduplication via condition variables.

## Key Types and Functions

### Data Structures

```c
typedef enum {
    AV_IDEM_NEW,        /* Key is unregistered; caller must execute the request */
    AV_IDEM_CACHED,     /* Matching request previously completed; cached response returned */
    AV_IDEM_WAIT,       /* Identical request currently in flight; caller should wait */
    AV_IDEM_CONFLICT    /* Key previously used with different request method, path, or payload */
} av_idem_state;

typedef struct {
    int status_code;            /* HTTP status (e.g. 200, 202) */
    char *response_body;        /* Serialized JSON response string */
    size_t response_len;
    uint64_t created_at_sec;    /* Monotonic timestamp */
} av_idem_record;

/* Forward declaration of idempotency manager */
typedef struct av_idempotency_mgr av_idempotency_mgr;
```

### Core Functions

| Function | Role |
| :--- | :--- |
| `av_idempotency_mgr_create` | Initializes the idempotency manager backed by LMDB store `store`. |
| `av_idempotency_check` | Hashes request parameters, inspects `idem` database, registers in-flight requests, and returns the current idempotency state. |
| `av_idempotency_complete` | Updates an in-flight entry with the completed HTTP status code and response payload, signaling waiting threads. |
| `av_idempotency_abort` | Removes an in-flight entry if processing failed with a non-retryable internal error. |
| `av_idempotency_record_free` | Releases memory allocated within an `av_idem_record`. |
| `av_idempotency_mgr_destroy` | Destroys the manager and cleans up synchronization primitives. |

### Function Signatures

```c
av_status av_idempotency_mgr_create(av_store *store, av_idempotency_mgr **out_mgr, av_err *e);

av_status av_idempotency_check(av_idempotency_mgr *mgr, const char *key,
                               const char *method, const char *path,
                               const uint8_t *body, size_t body_len,
                               av_idem_state *out_state, av_idem_record *out_rec,
                               av_err *e);

av_status av_idempotency_complete(av_idempotency_mgr *mgr, const char *key,
                                  int status_code, const char *response_body,
                                  size_t response_len, av_err *e);

void av_idempotency_abort(av_idempotency_mgr *mgr, const char *key);

void av_idempotency_record_free(av_idem_record *rec);

void av_idempotency_mgr_destroy(av_idempotency_mgr *mgr);
```

## Memory Ownership

- **Manager Handle**: Allocated on the heap; destroyed with `av_idempotency_mgr_destroy()`.
- **Cached Response Body**: When `av_idempotency_check` returns `AV_IDEM_CACHED`, the response string in `out_rec->response_body` is allocated via `malloc`; the caller owns it and must release it using `av_idempotency_record_free()`.
- **Store Reference**: Borrows the `av_store *` instance for the lifetime of the manager.

## Invariants

- **Payload Binding**: The stored idempotency record binds strictly to `SHA-256(method || ":" || path || ":" || body)`.
- **Conflict Rejection**: Re-submitting an `Idempotency-Key` with differing method, path, or body triggers `409 idempotency_conflict` and performs no mutations.
- **24-Hour Expiration**: Cached responses are valid for 24 hours (86,400 seconds); expired entries are purged during periodic maintenance.
- **In-Flight Deduplication**: Concurrent requests sharing the same key and payload do not execute parallel on-chain transactions; subsequent threads wait for the leader's result.

## Error Handling

- Request conflicts set `code = "idempotency_conflict"` and HTTP status 409.
- LMDB write failures during record insertion return `AV_ERR` and set `code = "internal"`.

## Thread Safety

- Thread-safe.
- In-flight request table and condition variables are protected by an internal `pthread_mutex_t`.
- Persistent reads and writes into the `idem` LMDB table are protected by LMDB's internal transactional concurrency controls.
