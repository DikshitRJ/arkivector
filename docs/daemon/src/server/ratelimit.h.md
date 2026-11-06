# `daemon/src/server/ratelimit.h`

Declares token bucket rate limiting interfaces.

## Functions
- `av_ratelimit_create(double rps, double burst)`: Instantiates a token bucket rate limiter.
- `av_ratelimit_destroy(av_ratelimit *rl)`: Frees bucket structures.
- `av_ratelimit_is_enabled(const av_ratelimit *rl)`: Checks if rate limiting is enabled.
- `av_ratelimit_check(av_ratelimit *rl, const char *client_id, double *retry_after, av_err *e)`: Consumes a token for a given client identifier or returns an error with required backoff time.
