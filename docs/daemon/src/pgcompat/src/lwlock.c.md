# daemon/src/pgcompat/src/lwlock.c

## Purpose

Implements PostgreSQL lightweight locks as a 4-byte atomic reader-writer lock. HNSW code uses these locks through `LWLockAcquire`.

## Lock word

- The top bit (`1 << 31`) is the exclusive flag.
- The low 31 bits count shared holders.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `LWLockInitialize` | Set the word to zero. |
| `LWLockAcquire` | Spin, then yield, until the lock is free. Record the lock in a per-thread stack. Raise `ERROR` when the stack is full (`MAX_SIMUL_LWLOCKS`). |
| `LWLockConditionalAcquire` | Try once. |
| `LWLockRelease` | Find the lock in the per-thread stack. Clear or decrement the word. |
| `av_pg_unwind_save` and `av_pg_unwind_restore` | Save the lock depth and the read-only flag. On restore, release the locks taken after the save. |
| `av_lwlock_readonly_enter` and `av_lwlock_readonly_leave` | Switch read-only mode on and off. |
| `LWLockReleaseAll` | Release every lock held by the thread. |

## Read-only mode

`av_lwlock_readonly_arena` is per thread. When it is true, `LWLockAcquire` returns at once, and the lock word is not written. Use this for a mapped read-only arena.

## Ownership and thread safety

- The per-thread stack holds the locks the thread owns.
- `LWLockRelease` of a lock that the thread does not hold raises `ERROR` unless read-only mode is on.
- `LW_WAIT_UNTIL_FREE` raises `ERROR`.

## Testing

`test/unit/test_pgcompat.c`: `lwlock_shared_and_readonly`, `error_releases_lwlocks`, `release_after_flag_change`.

## Notes

- A spin loop yields after 200 tries. A search thread must wait for the entry lock with pgvector's handshake (see `test/unit/test_hnsw.c` and the D0 spike notes).
