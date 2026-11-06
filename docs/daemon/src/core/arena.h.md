# daemon/src/core/arena.h

## Purpose

Bump (region) allocator. An arena hands out memory by moving a pointer inside large blocks. Memory is not freed one piece at a time. `av_arena_reset()` frees everything at once. The PostgreSQL memory contexts in `pgcompat` map onto arenas.

## Public API

| Function | Purpose |
| :--- | :--- |
| `av_arena *av_arena_create(const char *name, size_t block_size, size_t hard_cap)` | Create an arena. `hard_cap` 0 means no limit. The name is copied, up to 31 characters. |
| `void *av_arena_alloc(av_arena *a, size_t n, size_t align)` | Allocate `n` bytes. `align` is a power of two, or 0 for 16. Returns NULL at the hard cap. |
| `void *av_arena_calloc(av_arena *a, size_t n, size_t align)` | Same, zero-filled. |
| `char *av_arena_strdup(av_arena *a, const char *s)` | Copy a string into the arena. |
| `char *av_arena_strndup(av_arena *a, const char *s, size_t n)` | Copy `n` bytes and a NUL into the arena. |
| `void av_arena_reset(av_arena *a)` | Release all allocations. Keep the first block. |
| `void av_arena_destroy(av_arena *a)` | Free the arena and all its blocks. NULL is allowed. |
| `size_t av_arena_used(const av_arena *a)` | Bytes handed out since the last reset, including padding. |
| `size_t av_arena_reserved(const av_arena *a)` | Bytes held in blocks, including block headers. |
| `const char *av_arena_name(const av_arena *a)` | The name given at creation. |

## Ownership and thread safety

- The caller owns the arena and destroys it.
- Memory returned by an arena is valid until the next reset or destroy.
- An arena is not thread-safe. One thread owns an arena at a time.

## Testing

Covered by the pgcompat unit suite (`test/unit/test_pgcompat.c`), and through the codec tests that allocate from arenas.

## Notes

- Requests larger than a block get a block of their own.
- A request with `align` above 4096, or a non-power-of-two `align`, returns NULL.
