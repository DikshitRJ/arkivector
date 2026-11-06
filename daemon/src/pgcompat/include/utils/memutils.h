/*
 * utils/memutils.h (pgcompat stub): MemoryContext management API.
 * See utils/palloc.h for the model. ARKIVECTOR-owned.
 */
#ifndef MEMUTILS_H
#define MEMUTILS_H

#include "utils/palloc.h"

struct av_arena;

#define MaxAllocSize ((Size) 0x3fffffff) /* 1 gigabyte - 1 */
#define AllocSizeIsValid(size) ((Size) (size) <= MaxAllocSize)
#define MaxAllocHugeSize (SIZE_MAX / 2)
#define InvalidAllocSize SIZE_MAX
#define AllocHugeSizeIsValid(size) ((Size) (size) <= MaxAllocHugeSize)

extern __thread MemoryContext TopMemoryContext;
extern __thread MemoryContext ErrorContext;
extern __thread MemoryContext CurTransactionContext;

#define ALLOCSET_DEFAULT_MINSIZE 0
#define ALLOCSET_DEFAULT_INITSIZE (8 * 1024)
#define ALLOCSET_DEFAULT_MAXSIZE (8 * 1024 * 1024)
#define ALLOCSET_DEFAULT_SIZES ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE
#define ALLOCSET_SMALL_MINSIZE 0
#define ALLOCSET_SMALL_INITSIZE (1 * 1024)
#define ALLOCSET_SMALL_MAXSIZE (8 * 1024)
#define ALLOCSET_SMALL_SIZES ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE
#define ALLOCSET_START_SMALL_SIZES ALLOCSET_SMALL_MINSIZE, ALLOCSET_SMALL_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE

extern MemoryContext AllocSetContextCreateInternal(MemoryContext parent, const char *name, Size minContextSize,
                                                   Size initBlockSize, Size maxBlockSize);
#define AllocSetContextCreate AllocSetContextCreateInternal
#define GenerationContextCreate(parent, name, min, init, max) AllocSetContextCreateInternal(parent, name, min, init, max)

/*
 * Arkivector extension: create a context with a hard cap on total bytes (0 = none).
 * When the cap is hit, palloc raises ereport(ERROR, ERRCODE_OUT_OF_MEMORY).
 */
extern MemoryContext AvContextCreate(MemoryContext parent, const char *name, Size block_size, Size hard_cap);

extern void MemoryContextReset(MemoryContext context);
extern void MemoryContextResetOnly(MemoryContext context);
extern void MemoryContextResetChildren(MemoryContext context);
extern void MemoryContextDelete(MemoryContext context);
extern void MemoryContextDeleteChildren(MemoryContext context);
extern void MemoryContextSetParent(MemoryContext context, MemoryContext new_parent);
extern MemoryContext GetMemoryChunkContext(void *pointer);
extern Size GetMemoryChunkSpace(void *pointer);
extern MemoryContext MemoryContextGetParent(MemoryContext context);
extern bool MemoryContextIsEmpty(MemoryContext context);
extern Size MemoryContextMemAllocated(MemoryContext context, bool recurse);
extern void MemoryContextStats(MemoryContext context);
extern bool MemoryContextContains(MemoryContext context, void *pointer);
extern struct av_arena *AvContextArena(MemoryContext context);

#endif /* MEMUTILS_H */
