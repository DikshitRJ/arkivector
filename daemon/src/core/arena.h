/*
 * core/arena.h: bump (region) allocator.
 *
 * An arena hands out memory by bumping a pointer inside large blocks. Memory is
 * never freed piecemeal: av_arena_reset() releases everything at once, keeping the
 * first block for reuse. This is the allocation discipline of PostgreSQL memory
 * contexts, which the copied pgvector code expects (palloc maps onto arenas, see
 * pgcompat/src/palloc.c).
 *
 * Thread safety: an arena is NOT thread-safe. Each arena is owned by one thread
 * at a time (request arenas by a query worker, batch arenas by the applier).
 */
#ifndef AV_CORE_ARENA_H
#define AV_CORE_ARENA_H

#include <stddef.h>

typedef struct av_arena av_arena;

/*
 * Create an arena. block_size is the default block size (a request larger than a
 * block gets its own block). hard_cap limits the total bytes of all blocks; 0 means
 * unlimited. Returns NULL on allocation failure. The name is copied (up to 31 chars).
 * Ownership: the caller owns the arena and frees it with av_arena_destroy().
 */
av_arena *av_arena_create(const char *name, size_t block_size, size_t hard_cap);

/*
 * Allocate n bytes aligned to align (a power of two, 0 means 16). Returns NULL when
 * the hard cap would be exceeded or malloc fails. n == 0 returns a valid unique
 * pointer. The memory is owned by the arena and valid until reset/destroy.
 */
void *av_arena_alloc(av_arena *a, size_t n, size_t align);

/* Same as av_arena_alloc, zero-filled. */
void *av_arena_calloc(av_arena *a, size_t n, size_t align);

/* Copy a NUL-terminated string (or n bytes + NUL with av_arena_strndup) into the arena. */
char *av_arena_strdup(av_arena *a, const char *s);
char *av_arena_strndup(av_arena *a, const char *s, size_t n);

/* Release every allocation. The first block is kept for reuse. */
void av_arena_reset(av_arena *a);

/* Free the arena and all its memory. NULL is a no-op. */
void av_arena_destroy(av_arena *a);

/* Statistics: bytes handed out since the last reset, and bytes held in blocks. */
size_t av_arena_used(const av_arena *a);
size_t av_arena_reserved(const av_arena *a);
const char *av_arena_name(const av_arena *a);

#endif /* AV_CORE_ARENA_H */
