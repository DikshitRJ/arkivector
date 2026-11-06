# `daemon/src/server/ratelimit.c`

Implements token bucket rate limiting per client ID / IP.

## Details
- Maintains a thread-safe hash table of token buckets keyed by IP or Authorization token.
- Tokens regenerate smoothly based on elapsed monotonic time (`clock_gettime(CLOCK_MONOTONIC)`).
- When a burst limit is exceeded, rejects requests with `429 rate_limited` and populates the `Retry-After` HTTP header with the exact wait duration in seconds.
