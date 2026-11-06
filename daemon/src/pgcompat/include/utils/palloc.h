/*
 * utils/palloc.h (pgcompat stub): palloc and MemoryContext on top of av_arena.
 *
 * A MemoryContext wraps one av_arena (core/arena.h). Every chunk carries a 16-byte
 * header {context, size}, so GetMemoryChunkContext() and repalloc() work as in
 * PostgreSQL. pfree() is a no-op: memory is reclaimed by MemoryContextReset/Delete,
 * which matches pgvector's "allocate freely, reset the context" discipline.
 * Contexts form a tree; resetting or deleting a context deletes its children
 * (PostgreSQL semantics). CurrentMemoryContext is thread-local. If a thread pallocs
 * without setting one, a per-thread TopMemoryContext is created on first use.
 *
 * Thread safety: a context belongs to one thread at a time. ARKIVECTOR-owned.
 */
#ifndef PALLOC_H
#define PALLOC_H

typedef struct MemoryContextData *MemoryContext;

typedef void (*MemoryContextCallbackFunction)(void *arg);
typedef struct MemoryContextCallback {
    MemoryContextCallbackFunction func;
    void *arg;
    struct MemoryContextCallback *next;
} MemoryContextCallback;

extern __thread MemoryContext CurrentMemoryContext;

#define MCXT_ALLOC_HUGE   0x01
#define MCXT_ALLOC_NO_OOM 0x02
#define MCXT_ALLOC_ZERO   0x04

extern void *MemoryContextAlloc(MemoryContext context, Size size);
extern void *MemoryContextAllocZero(MemoryContext context, Size size);
extern void *MemoryContextAllocExtended(MemoryContext context, Size size, int flags);
extern void *MemoryContextAllocAligned(MemoryContext context, Size size, Size alignto, int flags);
extern void *MemoryContextAllocHuge(MemoryContext context, Size size);

extern void *palloc(Size size);
extern void *palloc0(Size size);
extern void *palloc_extended(Size size, int flags);
extern void *palloc_aligned(Size size, Size alignto, int flags);
extern pg_nodiscard void *repalloc(void *pointer, Size size);
extern pg_nodiscard void *repalloc_extended(void *pointer, Size size, int flags);
extern pg_nodiscard void *repalloc0(void *pointer, Size oldsize, Size size);
extern pg_nodiscard void *repalloc_huge(void *pointer, Size size);
extern void pfree(void *pointer);

extern Size add_size(Size s1, Size s2);
extern Size mul_size(Size s1, Size s2);

#define palloc_object(type)                  ((type *) palloc(sizeof(type)))
#define palloc0_object(type)                 ((type *) palloc0(sizeof(type)))
#define palloc_array(type, count)            ((type *) palloc(mul_size(sizeof(type), count)))
#define palloc0_array(type, count)           ((type *) palloc0(mul_size(sizeof(type), count)))
#define repalloc_array(pointer, type, count) ((type *) repalloc(pointer, mul_size(sizeof(type), count)))
#define repalloc0_array(pointer, type, oldcount, count) \
    ((type *) repalloc0(pointer, mul_size(sizeof(type), oldcount), mul_size(sizeof(type), count)))

static inline MemoryContext MemoryContextSwitchTo(MemoryContext context)
{
    MemoryContext old = CurrentMemoryContext;
    CurrentMemoryContext = context;
    return old;
}

extern void MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb);

extern char *MemoryContextStrdup(MemoryContext context, const char *string);
extern char *pstrdup(const char *in);
extern char *pnstrdup(const char *in, Size len);
extern char *pchomp(const char *in);
extern char *psprintf(const char *fmt, ...) pg_attribute_printf(1, 2);
extern size_t pvsnprintf(char *buf, size_t len, const char *fmt, va_list args) pg_attribute_printf(3, 0);

#endif /* PALLOC_H */
