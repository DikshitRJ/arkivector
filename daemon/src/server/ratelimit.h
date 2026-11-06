/*
 * server/ratelimit.h: Token bucket rate limiter per key and per IP.
 */
#ifndef AV_SERVER_RATELIMIT_H
#define AV_SERVER_RATELIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

typedef struct av_ratelimit av_ratelimit;

/*
 * Create rate limiter.
 * rps: requests per second allowed per identifier.
 * burst: max burst capacity.
 * If rps <= 0, rate limiting is disabled.
 */
av_ratelimit *av_ratelimit_create(double rps, double burst);
void av_ratelimit_destroy(av_ratelimit *rl);

bool av_ratelimit_is_enabled(const av_ratelimit *rl);

/*
 * Check and consume 1 token for identifier (e.g., API key ID or client IP).
 * If token available, returns AV_OK.
 * If rate limited, returns AV_ERR with e->code = "rate_limited" and sets *retry_after_s.
 */
av_status av_ratelimit_check(av_ratelimit *rl, const char *identifier, int *retry_after_s, av_err *e);

#endif /* AV_SERVER_RATELIMIT_H */
