/*
 * arkiv/budget.h: RPC rate limiter (token bucket) with per-method compute-unit (CU) weights.
 *
 * The bucket refills at `per_sec` CU per second and holds at most max(per_sec, 8) CU of burst.
 * av_budget_acquire blocks the calling thread until the cost is available.
 * Thread-safe: one mutex guards the bucket; sleeping happens outside the lock.
 */
#ifndef AV_ARKIV_BUDGET_H
#define AV_ARKIV_BUDGET_H

#include <stdint.h>

typedef struct av_budget av_budget;

/* per_sec <= 0 disables limiting. Returns NULL on allocation failure. */
av_budget *av_budget_create(double per_sec);
void av_budget_destroy(av_budget *b);

/* Default CU weight of a JSON-RPC method (unknown methods cost 2). */
int av_budget_cost(const char *method);

/* Block until `cu` units are available, then take them. Returns the milliseconds waited. */
uint64_t av_budget_acquire(av_budget *b, int cu);

/* Total CU consumed and milliseconds spent waiting since creation (metrics). */
uint64_t av_budget_consumed(av_budget *b);
uint64_t av_budget_waited_ms(av_budget *b);

#endif /* AV_ARKIV_BUDGET_H */
