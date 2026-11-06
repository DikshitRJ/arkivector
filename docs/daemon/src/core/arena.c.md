# daemon/src/core/arena.c

## Purpose

Implements the bump allocator described in `arena.h`.

## Key functions

| Function | Behavior |
| :--- | :--- |
| `av_arena_create` | Allocate the control block with `calloc`. The default block size is 64 KiB. |
| `av_arena_alloc` | Try the head block first. If it does not fit, add a block. A request larger than the default block size gets a block of its own. |
| `av_arena_reset` | Free every block after the first. Reset `used` on the first block. |
| `av_arena_destroy` | Free every block, then the control block. |

Each block has a 16-byte-aligned header. A block holds `used` and `size`. The `reserved` count includes header sizes. A `hard_cap` check runs before each `malloc`.

## Ownership and thread safety

- `malloc` is used only for blocks. Blocks belong to the arena.
- No locks. Callers must not share an arena between threads.

## Testing

`test/unit/test_pgcompat.c` checks allocation, reset and the capped OOM path through `palloc`.

## Notes

- `av_arena_alloc` with `n == 0` reserves one byte, so the pointer is unique.
- Alignment padding counts in `used`.
