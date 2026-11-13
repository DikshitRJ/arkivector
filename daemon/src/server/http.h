/*
 * server/http.h: HTTP/1.1 server (libuv TCP + llhttp) and the request/response types handlers use.
 *
 * Threads: the server lives on the loop thread. Handlers run on the query pool (see
 * av_http_config.workers) and only touch the request, response and arena they are given.
 * Finished responses return to the loop through an mpsc queue plus uv_async_send; the loop
 * never blocks and never runs handlers.
 *
 * Limits: body cap (413 payload_too_large), header section cap (400 bad_request), request
 * receive timeout (slow-loris, connection closed), keep-alive idle timeout. A request body
 * that is not application/json gets 415 unsupported_media_type before the handler runs.
 *
 * Headers added to every response: X-Request-Id, Arkivector-Api-Version, and
 * Arkivector-As-Of-Block when resp->has_as_of.
 */
#ifndef AV_SERVER_HTTP_H
#define AV_SERVER_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#include "core/arena.h"
#include "log.h"

#define AV_HTTP_MAX_HEADER_BYTES 16384U /* request line + all headers */
#define AV_HTTP_MAX_HEADERS      100U

typedef struct av_http_req {
    const char *method; /* "GET", ... */
    const char *path;   /* raw (still percent-encoded), no query string */
    const char *query;  /* raw query string without '?', "" when absent */
    const char *body;   /* NUL-terminated, "" when absent */
    size_t body_len;
    const char *request_id;      /* echoed X-Request-Id or a generated ULID */
    const char *content_type;    /* NULL when absent */
    const char *authorization;   /* NULL when absent */
    const char *idempotency_key; /* NULL when absent */
    av_arena *arena;             /* request arena; reset after the response is sent */
    const char *origin;          /* Origin header, NULL when absent (MCP DNS-rebinding check) */
} av_http_req;

typedef struct av_http_resp {
    int status;               /* 0 is treated as 500 internal */
    const char *content_type; /* NULL means application/json */
    char *body;               /* malloc'd; the server frees it. May be NULL when body_len == 0 */
    size_t body_len;
    uint64_t as_of_block;
    bool has_as_of;
    const char *retry_after; /* seconds, or NULL */
    const char *allow;       /* Allow header (405), or NULL */
    const char *warning;     /* Warning header value (static or request-arena string), or NULL */
    const char *request_id;  /* pre-filled by the server; used by av_problem_from_err */
} av_http_resp;

/* Runs on a query worker. params: percent-decoded {param} values in pattern order. */
typedef void (*av_http_handler)(const av_http_req *req, const char *const *params, size_t n_params,
                                av_http_resp *resp, void *ctx);

/* ---- incremental request parser (no sockets; used by the server and by the fuzzer) ---- */

typedef struct av_httpp av_httpp;
typedef enum {
    AV_HTTPP_NEED_MORE = 0, /* all input consumed, request incomplete */
    AV_HTTPP_REQUEST = 1,   /* one complete request is ready; *consumed may be < len (pipelining) */
    AV_HTTPP_ERROR = 2      /* av_httpp_error() describes it; the connection must be closed after replying */
} av_httpp_result;

/* max_body: body cap in bytes. NULL on allocation failure. */
av_httpp *av_httpp_create(size_t max_body);
void av_httpp_destroy(av_httpp *p);
/* Feed bytes. Stops after the first complete request. */
av_httpp_result av_httpp_feed(av_httpp *p, const char *data, size_t len, size_t *consumed);
/* Valid after AV_HTTPP_REQUEST, until av_httpp_next. request_id is generated or echoed. */
const av_http_req *av_httpp_request(const av_httpp *p);
bool av_httpp_keep_alive(const av_httpp *p);
/* True once if the client sent "Expect: 100-continue" and the headers were accepted. */
bool av_httpp_take_continue(av_httpp *p);
/* True when a message has started (bytes seen) but is not complete. */
bool av_httpp_in_progress(const av_httpp *p);
/* After AV_HTTPP_ERROR: HTTP status and the av_err (code from spec/error-codes.json). */
int av_httpp_error(const av_httpp *p, av_err *e);
/* Reset for the next request on the connection (after the response was written). */
void av_httpp_next(av_httpp *p);

/* 26-character Crockford base32 ULID (plus NUL). */
void av_ulid_generate(char out[27]);
/* Valid client request id: 1..64 chars of [A-Za-z0-9._-]. */
bool av_request_id_valid(const char *s);

/* ---- server ---- */

struct av_router;

typedef struct av_http_config {
    const char *listen;        /* "host:port" or "[v6]:port" (port 0 = ephemeral) */
    size_t max_body;           /* bytes; 0 = 32 MiB */
    size_t workers;            /* query pool threads; 0 = 4 */
    size_t max_queue;          /* pending handler tasks; 0 = 4096 */
    size_t max_connections;    /* 0 = 1024 */
    int request_timeout_ms;    /* receive a full request within this; 0 = 30000 */
    int idle_timeout_ms;       /* keep-alive idle; 0 = 60000 */
    int drain_timeout_ms;      /* graceful shutdown wait for in-flight; 0 = 10000 */
    const char *api_keys_file; /* NULL or "" = auth disabled */
    double ratelimit_rps;      /* 0 = disabled */
    double ratelimit_burst;
    struct av_router *router; /* not owned */
} av_http_config;

typedef struct av_http_server av_http_server;

av_http_server *av_http_create(uv_loop_t *loop, const av_http_config *cfg, av_err *e);
av_status av_http_listen(av_http_server *s, av_err *e);
/* Bound TCP port (after av_http_listen), 0 if not listening. */
int av_http_port(const av_http_server *s);
struct av_auth;
/* The API-key context (NULL when auth is disabled). Owned by the server; valid until av_http_destroy. */
const struct av_auth *av_http_auth(const av_http_server *s);

/*
 * Graceful shutdown on the loop thread: stop accepting, close idle connections, let in-flight
 * requests finish (their responses carry Connection: close), force-close after drain_timeout_ms,
 * then close every libuv handle and call done(ctx). Run the loop until it returns, then
 * av_http_destroy.
 */
void av_http_shutdown(av_http_server *s, void (*done)(void *ctx), void *ctx);
void av_http_destroy(av_http_server *s);

#endif /* AV_SERVER_HTTP_H */
