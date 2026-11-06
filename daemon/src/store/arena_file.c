/*
 * store/arena_file.c: see arena_file.h.
 */
#include "store/arena_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GROW_CHUNK ((size_t) 1 << 20) /* commit granularity: 1 MiB */

struct av_arena_file {
    uint8_t *base;
    size_t reserved;
    int fd;           /* -1 for anonymous */
    bool read_only;
    _Atomic size_t committed;
    _Atomic size_t used;
};

static size_t page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t) p : 4096;
}

static av_status sys_fail(av_err *e, const char *what, const char *path)
{
    av_err_set(e, "internal", "%s %s: %s", what, path != NULL ? path : "(anonymous)", strerror(errno));
    return AV_ERR;
}

av_status av_arena_file_open(const char *path, size_t reserve, unsigned flags, av_arena_file **out, av_err *e)
{
    size_t ps = page_size();
    bool ro = (flags & AV_AF_READONLY) != 0;
    if (ro && path == NULL) {
        av_err_set(e, "internal", "read-only arena file needs a path");
        return AV_ERR;
    }
    av_arena_file *f = calloc(1, sizeof *f);
    if (f == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    f->fd = -1;
    f->read_only = ro;
    size_t file_size = 0;
    if (path != NULL) {
        f->fd = open(path, ro ? O_RDONLY : (O_RDWR | O_CREAT | ((flags & AV_AF_TRUNCATE) ? O_TRUNC : 0)), 0644);
        if (f->fd < 0) {
            av_status st = sys_fail(e, "open", path);
            free(f);
            return st;
        }
        struct stat sb;
        if (fstat(f->fd, &sb) != 0) {
            av_status st = sys_fail(e, "fstat", path);
            close(f->fd);
            free(f);
            return st;
        }
        file_size = (size_t) sb.st_size;
    }
    size_t want = ro ? file_size : reserve;
    want = (want + ps - 1) / ps * ps;
    if (want == 0)
        want = ps;
    size_t min_want = ro ? want : (want / 64 < ps ? ps : want / 64 / ps * ps);
    void *p = MAP_FAILED;
    size_t sz = want;
    for (;;) {
        p = mmap(NULL, sz, ro ? PROT_READ : PROT_NONE, ro ? MAP_SHARED : (MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE),
                 ro ? f->fd : -1, 0);
        if (p != MAP_FAILED || ro)
            break;
        size_t half = sz / 2 / ps * ps;
        if (half < min_want || half == 0)
            break;
        sz = half;
    }
    if (p != MAP_FAILED)
        want = sz;
    if (p == MAP_FAILED) {
        av_status st = sys_fail(e, "mmap reserve", path);
        if (f->fd >= 0)
            close(f->fd);
        free(f);
        return st;
    }
    f->base = p;
    f->reserved = want;
    if (ro) {
        atomic_store(&f->committed, file_size);
        atomic_store(&f->used, file_size);
    } else if (f->fd >= 0 && file_size > 0 && !(flags & AV_AF_TRUNCATE)) {
        /* Reopen for append: keep existing bytes, committed up to the file size. */
        size_t keep = file_size > want ? want : file_size;
        if (mmap(f->base, keep, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, f->fd, 0) == MAP_FAILED) {
            av_status st = sys_fail(e, "mmap existing", path);
            av_arena_file_close(f);
            return st;
        }
        atomic_store(&f->committed, keep);
        atomic_store(&f->used, keep);
    }
    *out = f;
    return AV_OK;
}

void av_arena_file_close(av_arena_file *f)
{
    if (f == NULL)
        return;
    if (f->base != NULL)
        munmap(f->base, f->reserved);
    if (f->fd >= 0)
        close(f->fd);
    free(f);
}

av_status av_arena_file_ensure(av_arena_file *f, size_t n, av_err *e)
{
    size_t committed = atomic_load_explicit(&f->committed, memory_order_relaxed);
    if (n <= committed)
        return AV_OK;
    if (f->read_only || n > f->reserved) {
        av_err_set(e, "internal", "arena file %s", f->read_only ? "is read-only" : "reservation exhausted");
        return AV_ERR;
    }
    size_t ps = page_size();
    size_t chunk = GROW_CHUNK < ps ? ps : GROW_CHUNK;
    size_t target = (n + chunk - 1) / chunk * chunk;
    if (target > f->reserved)
        target = f->reserved;
    if (f->fd >= 0) {
        if (ftruncate(f->fd, (off_t) target) != 0)
            return sys_fail(e, "ftruncate", NULL);
        if (mmap(f->base + committed, target - committed, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, f->fd,
                 (off_t) committed) == MAP_FAILED)
            return sys_fail(e, "mmap grow", NULL);
    } else if (mprotect(f->base + committed, target - committed, PROT_READ | PROT_WRITE) != 0) {
        return sys_fail(e, "mprotect grow", NULL);
    }
    atomic_store_explicit(&f->committed, target, memory_order_release);
    return AV_OK;
}

void *av_arena_file_alloc(av_arena_file *f, size_t n, size_t align, av_err *e)
{
    if (align == 0)
        align = 8;
    if ((align & (align - 1)) != 0 || f->read_only) {
        av_err_set(e, "internal", "arena file: bad alignment or read-only");
        return NULL;
    }
    size_t used = atomic_load_explicit(&f->used, memory_order_relaxed);
    size_t start = (used + align - 1) & ~(align - 1);
    if (start < used || n > f->reserved || start > f->reserved - n) {
        av_err_set(e, "internal", "arena file reservation exhausted (%zu bytes)", f->reserved);
        return NULL;
    }
    if (av_arena_file_ensure(f, start + n, e) != AV_OK)
        return NULL;
    atomic_store_explicit(&f->used, start + n, memory_order_release);
    return f->base + start;
}

void *av_arena_file_base(const av_arena_file *f)
{
    return f->base;
}

size_t av_arena_file_used(const av_arena_file *f)
{
    return atomic_load_explicit(&f->used, memory_order_acquire);
}

size_t av_arena_file_committed(const av_arena_file *f)
{
    return atomic_load_explicit(&f->committed, memory_order_acquire);
}

size_t av_arena_file_reserved(const av_arena_file *f)
{
    return f->reserved;
}

av_status av_arena_file_sync(av_arena_file *f, av_err *e)
{
    if (f->fd < 0 || f->read_only)
        return AV_OK;
    size_t used = av_arena_file_used(f);
    if (ftruncate(f->fd, (off_t) used) != 0)
        return sys_fail(e, "ftruncate", NULL);
    size_t ps = page_size();
    size_t len = (used + ps - 1) / ps * ps;
    if (len > 0 && msync(f->base, len, MS_SYNC) != 0)
        return sys_fail(e, "msync", NULL);
    if (fsync(f->fd) != 0)
        return sys_fail(e, "fsync", NULL);
    return AV_OK;
}
