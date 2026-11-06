/*
 * arkiv/budget.c: token bucket. See budget.h.
 */
#include "arkiv/budget.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AV_BUDGET_MAX_COST 8

struct av_budget {
    pthread_mutex_t mu;
    double per_sec; /* <= 0: unlimited */
    double cap;
    double tokens;
    uint64_t last_ns;
    uint64_t consumed;
    uint64_t waited_ms;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

av_budget *av_budget_create(double per_sec)
{
    av_budget *b = calloc(1, sizeof *b);
    if (b == NULL)
        return NULL;
    pthread_mutex_init(&b->mu, NULL);
    b->per_sec = per_sec;
    b->cap = per_sec > (double) AV_BUDGET_MAX_COST ? per_sec : (double) AV_BUDGET_MAX_COST;
    b->tokens = b->cap;
    b->last_ns = now_ns();
    return b;
}

void av_budget_destroy(av_budget *b)
{
    if (b == NULL)
        return;
    pthread_mutex_destroy(&b->mu);
    free(b);
}

int av_budget_cost(const char *method)
{
    static const struct {
        const char *m;
        int cu;
    } tab[] = {{"eth_blockNumber", 1},      {"eth_chainId", 1},           {"arkiv_getBlockTiming", 1},
               {"eth_getBlockByNumber", 2}, {"arkiv_getEntity", 2},       {"arkiv_query", 5},
               {"eth_getLogs", 5},          {"eth_sendRawTransaction", 3}};
    if (method != NULL)
        for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
            if (strcmp(tab[i].m, method) == 0)
                return tab[i].cu;
    return 2;
}

uint64_t av_budget_acquire(av_budget *b, int cu)
{
    uint64_t waited = 0;
    if (b == NULL || b->per_sec <= 0 || cu <= 0)
        return 0;
    double need = cu > AV_BUDGET_MAX_COST ? (double) AV_BUDGET_MAX_COST : (double) cu;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        uint64_t t = now_ns();
        b->tokens += (double) (t - b->last_ns) / 1e9 * b->per_sec;
        if (b->tokens > b->cap)
            b->tokens = b->cap;
        b->last_ns = t;
        if (b->tokens >= need) {
            b->tokens -= need;
            b->consumed += (uint64_t) need;
            b->waited_ms += waited;
            pthread_mutex_unlock(&b->mu);
            return waited;
        }
        double wait_s = (need - b->tokens) / b->per_sec;
        pthread_mutex_unlock(&b->mu);
        struct timespec ts;
        ts.tv_sec = (time_t) wait_s;
        ts.tv_nsec = (long) ((wait_s - (double) ts.tv_sec) * 1e9) + 1000000L; /* +1 ms to avoid spinning */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        nanosleep(&ts, NULL);
        waited += (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
    }
}

uint64_t av_budget_consumed(av_budget *b)
{
    if (b == NULL)
        return 0;
    pthread_mutex_lock(&b->mu);
    uint64_t v = b->consumed;
    pthread_mutex_unlock(&b->mu);
    return v;
}

uint64_t av_budget_waited_ms(av_budget *b)
{
    if (b == NULL)
        return 0;
    pthread_mutex_lock(&b->mu);
    uint64_t v = b->waited_ms;
    pthread_mutex_unlock(&b->mu);
    return v;
}
