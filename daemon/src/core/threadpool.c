/*
 * core/threadpool.c: fixed pthread pool. See threadpool.h.
 */
#include "core/threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct task {
    av_task_fn fn;
    void *arg;
    struct task *next;
} task;

struct av_threadpool {
    char name[32];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    task *head;
    task *tail;
    size_t queued;
    size_t max_queue;
    bool stopping;
    pthread_t *threads;
    size_t n_started;
    size_t n_threads;
};

static void *worker_main(void *arg)
{
    av_threadpool *p = (av_threadpool *) arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (p->head == NULL && !p->stopping) {
            pthread_cond_wait(&p->cv, &p->mu);
        }
        if (p->head == NULL) {
            pthread_mutex_unlock(&p->mu);
            return NULL; /* stopping and drained */
        }
        task *t = p->head;
        p->head = t->next;
        if (p->head == NULL) {
            p->tail = NULL;
        }
        p->queued--;
        pthread_mutex_unlock(&p->mu);
        t->fn(t->arg);
        free(t);
    }
}

av_threadpool *av_threadpool_create(const char *name, size_t n_threads, size_t max_queue)
{
    if (n_threads == 0 || max_queue == 0) {
        return NULL;
    }
    av_threadpool *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    snprintf(p->name, sizeof(p->name), "%s", name != NULL ? name : "pool");
    p->max_queue = max_queue;
    p->n_threads = n_threads;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->threads = calloc(n_threads, sizeof(pthread_t));
    if (p->threads == NULL) {
        av_threadpool_destroy(p);
        return NULL;
    }
    for (size_t i = 0; i < n_threads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker_main, p) != 0) {
            break;
        }
        p->n_started++;
    }
    if (p->n_started == 0) {
        av_threadpool_destroy(p);
        return NULL;
    }
    return p;
}

av_status av_threadpool_submit(av_threadpool *p, av_task_fn fn, void *arg, av_err *e)
{
    task *t = malloc(sizeof(*t));
    if (t == NULL) {
        av_err_set(e, "internal", "out of memory queueing task on pool %s", p->name);
        return AV_ERR;
    }
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;
    pthread_mutex_lock(&p->mu);
    if (p->stopping || p->queued >= p->max_queue) {
        bool stopping = p->stopping;
        pthread_mutex_unlock(&p->mu);
        free(t);
        av_err_set(e, "not_ready", stopping ? "pool %s is shutting down" : "pool %s queue is full", p->name);
        return AV_ERR;
    }
    if (p->tail != NULL) {
        p->tail->next = t;
    } else {
        p->head = t;
    }
    p->tail = t;
    p->queued++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return AV_OK;
}

size_t av_threadpool_queued(av_threadpool *p)
{
    pthread_mutex_lock(&p->mu);
    size_t n = p->queued;
    pthread_mutex_unlock(&p->mu);
    return n;
}

void av_threadpool_shutdown(av_threadpool *p)
{
    pthread_mutex_lock(&p->mu);
    bool first = !p->stopping;
    p->stopping = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    if (first) {
        for (size_t i = 0; i < p->n_started; i++) {
            pthread_join(p->threads[i], NULL);
        }
    }
}

void av_threadpool_destroy(av_threadpool *p)
{
    if (p == NULL) {
        return;
    }
    av_threadpool_shutdown(p);
    free(p->threads);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p);
}
