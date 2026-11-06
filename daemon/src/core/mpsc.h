/*
 * core/mpsc.h: intrusive multi-producer / single-consumer queue (Vyukov) with a libuv wakeup.
 *
 * Thread safety: av_mpsc_push may be called from any thread. av_mpsc_pop and the wakeup
 * callback run on one consumer only (the loop thread for av_mpsc_wake).
 * Producers publish the node before calling uv_async_send, so a callback that drains the
 * queue until pop returns NULL sees every node whose push has returned.
 * Ownership: the queue never allocates; nodes belong to the caller. Embed av_mpsc_node as the
 * first member of the payload struct.
 */
#ifndef AV_CORE_MPSC_H
#define AV_CORE_MPSC_H

#include <stdatomic.h>
#include <uv.h>

#include "log.h"

typedef struct av_mpsc_node {
    _Atomic(struct av_mpsc_node *) next;
} av_mpsc_node;

typedef struct av_mpsc {
    _Atomic(av_mpsc_node *) head; /* producers swap here */
    av_mpsc_node *tail;           /* consumer only */
    av_mpsc_node stub;
} av_mpsc;

void av_mpsc_init(av_mpsc *q);
/* Any thread. */
void av_mpsc_push(av_mpsc *q, av_mpsc_node *n);
/* Consumer only. NULL when empty (or when a producer is mid-push: its wakeup will come). */
av_mpsc_node *av_mpsc_pop(av_mpsc *q);

/* Queue + uv_async_t: producers call av_mpsc_wake_push, the loop calls cb after the push. */
typedef struct av_mpsc_wake av_mpsc_wake;
typedef void (*av_mpsc_wake_cb)(av_mpsc_wake *w, void *ctx);
struct av_mpsc_wake {
    av_mpsc q;
    uv_async_t async;
    av_mpsc_wake_cb cb;
    void *ctx;
    bool open;
};

/* Registers the async handle on loop. cb runs on the loop thread; drain with av_mpsc_pop until NULL. */
av_status av_mpsc_wake_init(av_mpsc_wake *w, uv_loop_t *loop, av_mpsc_wake_cb cb, void *ctx, av_err *e);
/* Any thread, while the wake object is open. */
void av_mpsc_wake_push(av_mpsc_wake *w, av_mpsc_node *n);
/* Loop thread: close the async handle. close_cb (optional) runs after libuv released it. */
void av_mpsc_wake_close(av_mpsc_wake *w, uv_close_cb close_cb);

#endif /* AV_CORE_MPSC_H */
