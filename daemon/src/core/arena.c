/*
 * core/arena.c: bump allocator implementation. See arena.h.
 */
#include "core/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct av_arena_block {
    struct av_arena_block *next;
    size_t size; /* usable bytes after the header */
    size_t used;
    /* data follows, aligned to 16 */
} av_arena_block;

#define AV_ARENA_HDR           (((sizeof(av_arena_block) + 15u) / 16u) * 16u)
#define AV_ARENA_DEFAULT_BLOCK ((size_t) 64 * 1024)

struct av_arena {
    av_arena_block *head;  /* current block (most recent) */
    av_arena_block *first; /* oldest block, kept across resets */
    size_t block_size;
    size_t hard_cap;
    size_t reserved; /* sum of block sizes incl. headers */
    size_t used;     /* bytes handed out (incl. alignment padding) */
    char name[32];
};

static av_arena_block *block_new(av_arena *a, size_t min_size)
{
    size_t size = a->block_size;
    if (min_size > size) {
        size = min_size;
    }
    if (size > SIZE_MAX - AV_ARENA_HDR) {
        return NULL;
    }
    size_t total = AV_ARENA_HDR + size;
    if (a->hard_cap != 0 && (total > a->hard_cap || a->reserved > a->hard_cap - total)) {
        return NULL;
    }
    av_arena_block *b = malloc(total);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->size = size;
    b->used = 0;
    a->reserved += total;
    return b;
}

av_arena *av_arena_create(const char *name, size_t block_size, size_t hard_cap)
{
    av_arena *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }
    a->block_size = block_size ? block_size : AV_ARENA_DEFAULT_BLOCK;
    a->hard_cap = hard_cap;
    if (name != NULL) {
        strncpy(a->name, name, sizeof(a->name) - 1);
    }
    return a;
}

static inline uintptr_t align_up(uintptr_t p, size_t align)
{
    return (p + (align - 1)) & ~((uintptr_t) align - 1);
}

void *av_arena_alloc(av_arena *a, size_t n, size_t align)
{
    if (a == NULL) {
        return NULL;
    }
    if (align == 0) {
        align = 16;
    }
    if ((align & (align - 1)) != 0 || align > 4096) {
        return NULL;
    }
    if (n == 0) {
        n = 1;
    }
    av_arena_block *b = a->head;
    if (b != NULL) {
        uintptr_t base = (uintptr_t) b + AV_ARENA_HDR;
        uintptr_t cur = base + b->used;
        uintptr_t p = align_up(cur, align);
        size_t pad = (size_t) (p - cur);
        if (b->used + pad <= b->size && n <= b->size - b->used - pad) {
            b->used += pad + n;
            a->used += pad + n;
            return (void *) p;
        }
    }
    /* New block; large requests get a dedicated block sized to fit. */
    if (n > SIZE_MAX - align) {
        return NULL;
    }
    av_arena_block *nb = block_new(a, n + align);
    if (nb == NULL) {
        return NULL;
    }
    if (a->first == NULL) {
        a->first = nb;
        a->head = nb;
    } else {
        /* push on head; keep 'first' at the tail of the chain */
        nb->next = a->head;
        a->head = nb;
    }
    uintptr_t base = (uintptr_t) nb + AV_ARENA_HDR;
    uintptr_t p = align_up(base, align);
    size_t pad = (size_t) (p - base);
    nb->used = pad + n;
    a->used += pad + n;
    return (void *) p;
}

void *av_arena_calloc(av_arena *a, size_t n, size_t align)
{
    void *p = av_arena_alloc(a, n, align);
    if (p != NULL) {
        memset(p, 0, n ? n : 1);
    }
    return p;
}

char *av_arena_strndup(av_arena *a, const char *s, size_t n)
{
    char *d = av_arena_alloc(a, n + 1, 1);
    if (d == NULL) {
        return NULL;
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *av_arena_strdup(av_arena *a, const char *s)
{
    return av_arena_strndup(a, s, strlen(s));
}

void av_arena_reset(av_arena *a)
{
    if (a == NULL) {
        return;
    }
    av_arena_block *b = a->head;
    while (b != NULL && b != a->first) {
        av_arena_block *next = b->next;
        a->reserved -= AV_ARENA_HDR + b->size;
        free(b);
        b = next;
    }
    a->head = a->first;
    if (a->first != NULL) {
        a->first->used = 0;
        a->first->next = NULL;
    }
    a->used = 0;
}

void av_arena_destroy(av_arena *a)
{
    if (a == NULL) {
        return;
    }
    av_arena_block *b = a->head;
    while (b != NULL) {
        av_arena_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

size_t av_arena_used(const av_arena *a)
{
    return a ? a->used : 0;
}

size_t av_arena_reserved(const av_arena *a)
{
    return a ? a->reserved : 0;
}

const char *av_arena_name(const av_arena *a)
{
    return a ? a->name : "";
}
