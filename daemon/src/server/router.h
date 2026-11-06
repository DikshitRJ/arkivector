/*
 * server/router.h: method + path routing.
 *
 * Patterns are '/'-separated segments: a literal segment matches exactly, "{name}" matches any
 * non-empty segment and is returned percent-decoded. Colon verbs are part of a literal segment
 * ("/v1/collections:follow", "/v1/collections/{name}/search:batch"), so they never collide with
 * "{param}" routes. Among routes that match a path, the one with more literal segments wins.
 * No match: 404 route_not_found; path matches but not the method: 405 with Allow.
 *
 * Thread safety: av_router_add before serving only; av_router_match is read-only and thread-safe.
 */
#ifndef AV_SERVER_ROUTER_H
#define AV_SERVER_ROUTER_H

#include <stddef.h>

#include "core/arena.h"
#include "server/http.h"

#define AV_ROUTER_MAX_PARAMS   4
#define AV_ROUTER_MAX_SEGMENTS 16

typedef struct av_router av_router;

typedef struct av_route_match {
    int status;              /* 0 matched, 404 or 405 */
    av_http_handler handler; /* set when status == 0 */
    void *ctx;
    const char *pattern;                      /* matched pattern (low-cardinality metric label), "" if none */
    const char *params[AV_ROUTER_MAX_PARAMS]; /* arena strings */
    size_t n_params;
    char allow[96];   /* 405: "GET, POST" */
    int bad_encoding; /* 1 when a param has invalid percent-encoding (answer 400) */
} av_route_match;

av_router *av_router_create(void);
void av_router_destroy(av_router *r);

/* method: "GET" etc. pattern must start with '/', have at most AV_ROUTER_MAX_PARAMS params, and
 * must not contain empty segments. Duplicate (method, pattern) is rejected. */
av_status av_router_add(av_router *r, const char *method, const char *pattern, av_http_handler h, void *ctx,
                        av_err *e);

/* Percent-decode into arena memory. NULL on invalid encoding (bad hex, NUL byte) or OOM. */
char *av_percent_decode(av_arena *a, const char *s, size_t len);

/* path: raw request path (no query). Params are decoded into arena a. */
void av_router_match(const av_router *r, const char *method, const char *path, av_arena *a,
                     av_route_match *m);

#endif /* AV_SERVER_ROUTER_H */
