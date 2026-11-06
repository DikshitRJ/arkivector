# daemon/src/pgcompat/src/palloc.c

## Purpose

Implements the PostgreSQL `MemoryContext` and `palloc` API on top of `av_arena`. Copied pgvector code calls these functions. Our code does not.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `AvContextCreate(parent, name, block_size, hard_cap)` | Create a context with its own arena. Link it under `parent`. OOM raises `ERROR`. |
| `AllocSetContextCreateInternal` | PostgreSQL-shaped creator. Calls `AvContextCreate`. |
| `palloc`, `palloc0`, `palloc_extended`, `palloc_aligned` | Allocate in `CurrentMemoryContext`. A 16-byte header stores the owning context and the size. |
| `MemoryContextAlloc*` | Allocate in a named context. |
| `repalloc*` | Shrink in place, or allocate and copy. |
| `pfree` | No-op. Memory is freed at reset or delete. |
| `MemoryContextReset`, `MemoryContextDelete` | Run reset callbacks, delete children, release the arena. |
| `add_size`, `mul_size` | Overflow-checked. Raise `ERROR` on overflow. |
| `pstrdup`, `pnstrdup`, `psprintf`, `pvsnprintf` | Copy or format into the current context. |

Thread-local globals: `CurrentMemoryContext`, `TopMemoryContext`, `ErrorContext` and `CurTransactionContext`.

## Ownership and thread safety

- Each context owns its arena. Deleting a context frees everything in it.
- The globals are per thread (`__thread`). A thread must not use another thread's context without a lock.

## Testing

`test/unit/test_pgcompat.c`: `palloc_in_context`, `reset_frees_everything`, `oom_in_capped_context`.

## Notes

- Chunks are 16-byte aligned. A chunk header sits right before the returned pointer.
- `MCXT_ALLOC_NO_OOM` returns NULL on failure instead of raising `ERROR`.
