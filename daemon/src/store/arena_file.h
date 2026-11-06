/*
 * store/arena_file.h: a bump-allocated memory region with a stable base address.
 *
 * A virtual range of `reserve` bytes is mapped PROT_NONE up front. Growth commits more of
 * it: file-backed regions use ftruncate + mmap(MAP_FIXED | MAP_SHARED), anonymous ones
 * (path == NULL) use mprotect. The base address therefore never changes, so pointers (and
 * pgvector relptrs) into the region stay valid for the lifetime of the object.
 *
 * Thread safety: av_arena_file_alloc, av_arena_file_ensure and av_arena_file_sync are for
 * ONE writer thread (the applier). Readers may dereference pointers into already committed
 * bytes from any thread; the writer must publish new offsets to readers with a
 * synchronising operation (the collection's publish lock, or an atomic release store).
 * av_arena_file_used / _committed are atomic loads and safe from any thread.
 *
 * A read-only reopen (AV_AF_READONLY) maps the whole existing file PROT_READ and never grows.
 */
#ifndef AV_STORE_ARENA_FILE_H
#define AV_STORE_ARENA_FILE_H

#include <stdbool.h>
#include <stddef.h>

#include "log.h"

typedef struct av_arena_file av_arena_file;

#define AV_AF_READONLY 1u /* map an existing file read-only */
#define AV_AF_TRUNCATE 2u /* start empty even if the file exists */

/*
 * path NULL gives an anonymous region. reserve is rounded up to a page multiple; if the
 * kernel refuses the reservation (address-space limits, sanitizers) it is halved down to
 * 1/64 of the request, and av_arena_file_reserved() reports what was obtained.
 * Ownership: caller closes with av_arena_file_close().
 */
av_status av_arena_file_open(const char *path, size_t reserve, unsigned flags, av_arena_file **out, av_err *e);
void av_arena_file_close(av_arena_file *f);

/* Allocate n bytes aligned to align (power of two, 0 = 8). NULL (with e set) when the reservation is full. */
void *av_arena_file_alloc(av_arena_file *f, size_t n, size_t align, av_err *e);
/* Make sure bytes [0, n) are committed (writable). */
av_status av_arena_file_ensure(av_arena_file *f, size_t n, av_err *e);

void *av_arena_file_base(const av_arena_file *f);
size_t av_arena_file_used(const av_arena_file *f);
size_t av_arena_file_committed(const av_arena_file *f);
size_t av_arena_file_reserved(const av_arena_file *f);

/* msync the used range (file-backed only) and fsync the file. */
av_status av_arena_file_sync(av_arena_file *f, av_err *e);

#endif /* AV_STORE_ARENA_FILE_H */
