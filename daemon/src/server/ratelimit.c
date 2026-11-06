/*
 * server/ratelimit.c: Token bucket rate limiter.
 */
#include "server/ratelimit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RL_TABLE_SIZE 1024

typedef struct bucket {
    char id[64];
    double tokens;
    double last_time_sec;
    struct bucket *next;
} bucket;

struct av_ratelimit {
    bool enabled;
    double rps;
    double burst;
    pthread_mutex_t mu;
    bucket *table[RL_TABLE_SIZE];
};

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t) *s++;
        h *= 16777619u;
    }
    return h;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

av_ratelimit *av_ratelimit_create(double rps, double burst)
{
    av_ratelimit *rl = calloc(1, sizeof(*rl));
    if (rl == NULL) {
        return NULL;
    }
    if (rps <= 0.0) {
        rl->enabled = false;
        return rl;
    }
    rl->enabled = true;
    rl->rps = rps;
    rl->burst = burst > 0.0 ? burst : rps * 2.0;
    pthread_mutex_init(&rl->mu, NULL);
    return rl;
}

void av_ratelimit_destroy(av_ratelimit *rl)
{
    if (rl == NULL) {
        return;
    }
    pthread_mutex_lock(&rl->mu);
    for (size_t i = 0; i < RL_TABLE_SIZE; i++) {
        bucket *b = rl->table[i];
        while (b != NULL) {
            bucket *next = b->next;
            free(b);
            b = next;
        }
    }
    pthread_mutex_unlock(&rl->mu);
    pthread_mutex_destroy(&rl->mu);
    free(rl);
}

bool av_ratelimit_is_enabled(const av_ratelimit *rl)
{
    return rl != NULL && rl->enabled;
}

av_status av_ratelimit_check(av_ratelimit *rl, const char *identifier, int *retry_after_s, av_err *e)
{
    if (!av_ratelimit_is_enabled(rl) || identifier == NULL || identifier[0] == '\0') {
        return AV_OK;
    }

    uint32_t idx = hash_str(identifier) % RL_TABLE_SIZE;
    double t = now_sec();

    pthread_mutex_lock(&rl->mu);
    bucket *b = rl->table[idx];

    while (b != NULL) {
        if (strncmp(b->id, identifier, sizeof(b->id) - 1) == 0) {
            break;
        }
        b = b->next;
    }

    if (b == NULL) {
        b = calloc(1, sizeof(*b));
        if (b == NULL) {
            pthread_mutex_unlock(&rl->mu);
            return AV_OK; /* Soft fail on memory pressure */
        }
        strncpy(b->id, identifier, sizeof(b->id) - 1);
        b->tokens = rl->burst;
        b->last_time_sec = t;
        b->next = rl->table[idx];
        rl->table[idx] = b;
    }

    /* Replenish tokens */
    double delta = t - b->last_time_sec;
    b->last_time_sec = t;
    b->tokens += delta * rl->rps;
    if (b->tokens > rl->burst) {
        b->tokens = rl->burst;
    }

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        pthread_mutex_unlock(&rl->mu);
        return AV_OK;
    }

    /* Rate limited */
    double needed = 1.0 - b->tokens;
    double wait_s = needed / rl->rps;
    int after = (int) (wait_s + 0.999);
    if (after < 1)
        after = 1;

    if (retry_after_s != NULL) {
        *retry_after_s = after;
    }
    pthread_mutex_unlock(&rl->mu);

    av_err_set(e, "rate_limited", "rate limit exceeded; try again in %d seconds", after);
    return AV_ERR;
}
