/*
 * core/mpsc.c: Vyukov intrusive MPSC queue and the libuv wakeup. See mpsc.h.
 */
#include "core/mpsc.h"

void av_mpsc_init(av_mpsc *q)
{
    atomic_init(&q->stub.next, NULL);
    atomic_init(&q->head, &q->stub);
    q->tail = &q->stub;
}

void av_mpsc_push(av_mpsc *q, av_mpsc_node *n)
{
    atomic_store_explicit(&n->next, NULL, memory_order_relaxed);
    av_mpsc_node *prev = atomic_exchange_explicit(&q->head, n, memory_order_acq_rel);
    atomic_store_explicit(&prev->next, n, memory_order_release);
}

av_mpsc_node *av_mpsc_pop(av_mpsc *q)
{
    av_mpsc_node *tail = q->tail;
    av_mpsc_node *next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (tail == &q->stub) {
        if (next == NULL) {
            return NULL;
        }
        q->tail = next;
        tail = next;
        next = atomic_load_explicit(&tail->next, memory_order_acquire);
    }
    if (next != NULL) {
        q->tail = next;
        return tail;
    }
    av_mpsc_node *head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail != head) {
        return NULL; /* a producer is between exchange and link; its wakeup follows */
    }
    av_mpsc_push(q, &q->stub);
    next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (next != NULL) {
        q->tail = next;
        return tail;
    }
    return NULL;
}

static void wake_trampoline(uv_async_t *h)
{
    av_mpsc_wake *w = (av_mpsc_wake *) h->data;
    w->cb(w, w->ctx);
}

av_status av_mpsc_wake_init(av_mpsc_wake *w, uv_loop_t *loop, av_mpsc_wake_cb cb, void *ctx, av_err *e)
{
    av_mpsc_init(&w->q);
    w->cb = cb;
    w->ctx = ctx;
    w->async.data = w;
    int rc = uv_async_init(loop, &w->async, wake_trampoline);
    if (rc != 0) {
        av_err_set(e, "internal", "uv_async_init: %s", uv_strerror(rc));
        return AV_ERR;
    }
    w->open = true;
    return AV_OK;
}

void av_mpsc_wake_push(av_mpsc_wake *w, av_mpsc_node *n)
{
    av_mpsc_push(&w->q, n);
    uv_async_send(&w->async); /* thread-safe; coalesces */
}

void av_mpsc_wake_close(av_mpsc_wake *w, uv_close_cb close_cb)
{
    if (w->open) {
        w->open = false;
        uv_close((uv_handle_t *) &w->async, close_cb);
    }
}
