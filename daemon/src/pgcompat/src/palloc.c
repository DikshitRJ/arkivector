/*
 * pgcompat/src/palloc.c: PostgreSQL MemoryContext/palloc API on av_arena.
 * See utils/palloc.h for the model. ARKIVECTOR-owned.
 */
#include "postgres.h"

#include "core/arena.h"
#include "utils/memutils.h"

typedef struct MemoryContextData {
    av_arena *arena;
    struct MemoryContextData *parent;
    struct MemoryContextData *firstchild;
    struct MemoryContextData *prevchild;
    struct MemoryContextData *nextchild;
    MemoryContextCallback *reset_cbs;
    char name[32];
} MemoryContextData;

/* Every chunk is preceded by this header; 16 bytes keeps 16-byte alignment. */
typedef struct ChunkHeader {
    MemoryContext context;
    Size size;
} ChunkHeader;

StaticAssertDecl(sizeof(ChunkHeader) == 16, "chunk header must be 16 bytes");

__thread MemoryContext CurrentMemoryContext = NULL;
__thread MemoryContext TopMemoryContext = NULL;
__thread MemoryContext ErrorContext = NULL;
__thread MemoryContext CurTransactionContext = NULL;

static void context_link(MemoryContext ctx, MemoryContext parent)
{
    ctx->parent = parent;
    ctx->prevchild = NULL;
    ctx->nextchild = NULL;
    if (parent != NULL) {
        ctx->nextchild = parent->firstchild;
        if (parent->firstchild != NULL) {
            parent->firstchild->prevchild = ctx;
        }
        parent->firstchild = ctx;
    }
}

static void context_unlink(MemoryContext ctx)
{
    MemoryContext parent = ctx->parent;
    if (parent == NULL) {
        return;
    }
    if (ctx->prevchild != NULL) {
        ctx->prevchild->nextchild = ctx->nextchild;
    } else {
        parent->firstchild = ctx->nextchild;
    }
    if (ctx->nextchild != NULL) {
        ctx->nextchild->prevchild = ctx->prevchild;
    }
    ctx->parent = ctx->prevchild = ctx->nextchild = NULL;
}

MemoryContext AvContextCreate(MemoryContext parent, const char *name, Size block_size, Size hard_cap)
{
    MemoryContext ctx = calloc(1, sizeof(MemoryContextData));
    if (ctx == NULL) {
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
    }
    ctx->arena = av_arena_create(name, block_size ? block_size : 8192, hard_cap);
    if (ctx->arena == NULL) {
        free(ctx);
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
    }
    if (name != NULL) {
        strncpy(ctx->name, name, sizeof(ctx->name) - 1);
    }
    context_link(ctx, parent);
    return ctx;
}

MemoryContext AllocSetContextCreateInternal(MemoryContext parent, const char *name, Size minContextSize,
                                            Size initBlockSize, Size maxBlockSize)
{
    (void) minContextSize;
    Size block = initBlockSize;
    if (block < 1024) {
        block = 1024;
    }
    if (maxBlockSize > block && block < 64 * 1024) {
        block = Min(maxBlockSize, (Size) 64 * 1024);
    }
    return AvContextCreate(parent, name, block, 0);
}

static MemoryContext ensure_current(void)
{
    if (CurrentMemoryContext == NULL) {
        if (TopMemoryContext == NULL) {
            TopMemoryContext = AvContextCreate(NULL, "TopMemoryContext", 64 * 1024, 0);
        }
        CurrentMemoryContext = TopMemoryContext;
    }
    return CurrentMemoryContext;
}

static void run_reset_callbacks(MemoryContext ctx)
{
    MemoryContextCallback *cb;
    while ((cb = ctx->reset_cbs) != NULL) {
        ctx->reset_cbs = cb->next;
        cb->func(cb->arg);
    }
}

void MemoryContextDeleteChildren(MemoryContext context)
{
    while (context->firstchild != NULL) {
        MemoryContextDelete(context->firstchild);
    }
}

void MemoryContextResetOnly(MemoryContext context)
{
    run_reset_callbacks(context);
    av_arena_reset(context->arena);
}

void MemoryContextReset(MemoryContext context)
{
    if (context == NULL) {
        return;
    }
    MemoryContextDeleteChildren(context);
    MemoryContextResetOnly(context);
}

void MemoryContextResetChildren(MemoryContext context)
{
    for (MemoryContext c = context->firstchild; c != NULL; c = c->nextchild) {
        MemoryContextResetChildren(c);
        MemoryContextResetOnly(c);
    }
}

void MemoryContextDelete(MemoryContext context)
{
    if (context == NULL) {
        return;
    }
    MemoryContextDeleteChildren(context);
    run_reset_callbacks(context);
    context_unlink(context);
    if (CurrentMemoryContext == context) {
        CurrentMemoryContext = NULL;
    }
    if (TopMemoryContext == context) {
        TopMemoryContext = NULL;
    }
    av_arena_destroy(context->arena);
    free(context);
}

void MemoryContextSetParent(MemoryContext context, MemoryContext new_parent)
{
    context_unlink(context);
    context_link(context, new_parent);
}

MemoryContext MemoryContextGetParent(MemoryContext context)
{
    return context->parent;
}

bool MemoryContextIsEmpty(MemoryContext context)
{
    return context->firstchild == NULL && av_arena_used(context->arena) == 0;
}

Size MemoryContextMemAllocated(MemoryContext context, bool recurse)
{
    Size total = av_arena_reserved(context->arena);
    if (recurse) {
        for (MemoryContext c = context->firstchild; c != NULL; c = c->nextchild) {
            total += MemoryContextMemAllocated(c, true);
        }
    }
    return total;
}

void MemoryContextStats(MemoryContext context)
{
    fprintf(stderr, "%s: %zu bytes used, %zu reserved\n", context->name, av_arena_used(context->arena),
            av_arena_reserved(context->arena));
}

void MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
    cb->next = context->reset_cbs;
    context->reset_cbs = cb;
}

av_arena *AvContextArena(MemoryContext context)
{
    return context->arena;
}

static void *alloc_chunk(MemoryContext context, Size size, Size align, int flags)
{
    if (!(flags & MCXT_ALLOC_HUGE) && !AllocSizeIsValid(size)) {
        elog(ERROR, "invalid memory alloc request size %zu", size);
    }
    if (size > MaxAllocHugeSize) {
        elog(ERROR, "invalid memory alloc request size %zu", size);
    }
    if (align < 16) {
        align = 16;
    }
    /* Header sits immediately before the returned pointer; reserve 'align' bytes in front. */
    char *raw = av_arena_alloc(context->arena, size + align, align);
    if (raw == NULL) {
        if (flags & MCXT_ALLOC_NO_OOM) {
            return NULL;
        }
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory"),
                 errdetail("Failed on request of size %zu in memory context \"%s\".", size, context->name)));
    }
    char *p = raw + align;
    ChunkHeader *h = (ChunkHeader *) (p - sizeof(ChunkHeader));
    h->context = context;
    h->size = size;
    if (flags & MCXT_ALLOC_ZERO) {
        memset(p, 0, size);
    }
    return p;
}

void *MemoryContextAlloc(MemoryContext context, Size size)
{
    return alloc_chunk(context, size, 16, 0);
}

void *MemoryContextAllocZero(MemoryContext context, Size size)
{
    return alloc_chunk(context, size, 16, MCXT_ALLOC_ZERO);
}

void *MemoryContextAllocExtended(MemoryContext context, Size size, int flags)
{
    return alloc_chunk(context, size, 16, flags);
}

void *MemoryContextAllocAligned(MemoryContext context, Size size, Size alignto, int flags)
{
    return alloc_chunk(context, size, alignto, flags);
}

void *MemoryContextAllocHuge(MemoryContext context, Size size)
{
    return alloc_chunk(context, size, 16, MCXT_ALLOC_HUGE);
}

void *palloc(Size size)
{
    return alloc_chunk(ensure_current(), size, 16, 0);
}

void *palloc0(Size size)
{
    return alloc_chunk(ensure_current(), size, 16, MCXT_ALLOC_ZERO);
}

void *palloc_extended(Size size, int flags)
{
    return alloc_chunk(ensure_current(), size, 16, flags);
}

void *palloc_aligned(Size size, Size alignto, int flags)
{
    return alloc_chunk(ensure_current(), size, alignto, flags);
}

static ChunkHeader *chunk_header(void *pointer)
{
    return (ChunkHeader *) ((char *) pointer - sizeof(ChunkHeader));
}

MemoryContext GetMemoryChunkContext(void *pointer)
{
    return chunk_header(pointer)->context;
}

Size GetMemoryChunkSpace(void *pointer)
{
    return chunk_header(pointer)->size + sizeof(ChunkHeader);
}

bool MemoryContextContains(MemoryContext context, void *pointer)
{
    return pointer != NULL && GetMemoryChunkContext(pointer) == context;
}

void *repalloc_extended(void *pointer, Size size, int flags)
{
    ChunkHeader *h = chunk_header(pointer);
    if (size <= h->size) {
        h->size = size;
        return pointer;
    }
    void *n = alloc_chunk(h->context, size, 16, flags & ~MCXT_ALLOC_ZERO);
    if (n == NULL) {
        return NULL;
    }
    memcpy(n, pointer, h->size);
    return n;
}

void *repalloc(void *pointer, Size size)
{
    return repalloc_extended(pointer, size, 0);
}

void *repalloc_huge(void *pointer, Size size)
{
    return repalloc_extended(pointer, size, MCXT_ALLOC_HUGE);
}

void *repalloc0(void *pointer, Size oldsize, Size size)
{
    char *n = repalloc(pointer, size);
    if (size > oldsize) {
        memset(n + oldsize, 0, size - oldsize);
    }
    return n;
}

void pfree(void *pointer)
{
    (void) pointer; /* reclaimed at context reset/delete */
}

Size add_size(Size s1, Size s2)
{
    Size result = s1 + s2;
    if (result < s1 || result < s2) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("requested shared memory size overflows size_t")));
    }
    return result;
}

Size mul_size(Size s1, Size s2)
{
    Size result;
    if (s1 == 0 || s2 == 0) {
        return 0;
    }
    result = s1 * s2;
    if (result / s2 != s1) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("requested shared memory size overflows size_t")));
    }
    return result;
}

char *MemoryContextStrdup(MemoryContext context, const char *string)
{
    Size len = strlen(string) + 1;
    char *n = MemoryContextAlloc(context, len);
    memcpy(n, string, len);
    return n;
}

char *pstrdup(const char *in)
{
    return MemoryContextStrdup(ensure_current(), in);
}

char *pnstrdup(const char *in, Size len)
{
    len = strnlen(in, len);
    char *out = palloc(len + 1);
    memcpy(out, in, len);
    out[len] = '\0';
    return out;
}

char *pchomp(const char *in)
{
    size_t n = strlen(in);
    while (n > 0 && in[n - 1] == '\n') {
        n--;
    }
    return pnstrdup(in, n);
}

size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args)
{
    int nprinted = vsnprintf(buf, len, fmt, args);
    if (nprinted < 0) {
        elog(ERROR, "vsnprintf failed: %s", strerror(errno));
    }
    if ((size_t) nprinted < len) {
        return (size_t) nprinted;
    }
    return (size_t) nprinted + 1;
}

char *psprintf(const char *fmt, ...)
{
    size_t len = 128;
    for (;;) {
        char *result = palloc(len);
        va_list args;
        va_start(args, fmt);
        size_t newlen = pvsnprintf(result, len, fmt, args);
        va_end(args);
        if (newlen < len) {
            return result;
        }
        len = newlen;
    }
}
