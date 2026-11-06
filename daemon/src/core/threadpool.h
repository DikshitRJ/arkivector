/*
 * core/threadpool.h: fixed-size pthread pool with a bounded FIFO task queue.
 *
 * Thread safety: submit may be called from any thread. Tasks run on the pool threads, in
 * submission order per dequeue (no per-key ordering). A task must not block forever.
 * Ownership: the pool owns nothing of the task argument; the submitter keeps it alive until
 * the task has run.
 */
#ifndef AV_CORE_THREADPOOL_H
#define AV_CORE_THREADPOOL_H

#include <stddef.h>

#include "log.h"

typedef struct av_threadpool av_threadpool;
typedef void (*av_task_fn)(void *arg);

/* n_threads >= 1, max_queue >= 1 (queued, not yet running). name is copied (for logs). NULL on failure. */
av_threadpool *av_threadpool_create(const char *name, size_t n_threads, size_t max_queue);

/* AV_ERR (code "not_ready") when the queue is full or the pool is shutting down. */
av_status av_threadpool_submit(av_threadpool *p, av_task_fn fn, void *arg, av_err *e);

/* Tasks queued but not started. */
size_t av_threadpool_queued(av_threadpool *p);

/* Stop accepting tasks, run everything already queued, join the threads. Idempotent. */
void av_threadpool_shutdown(av_threadpool *p);

/* shutdown + free. NULL is a no-op. */
void av_threadpool_destroy(av_threadpool *p);

#endif /* AV_CORE_THREADPOOL_H */
