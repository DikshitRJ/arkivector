/*
 * server/http.c: HTTP/1.1 parser wrapper (llhttp) and the libuv TCP server. See http.h.
 *
 * Loop thread owns: server, connections, parsers, timers, all libuv handles.
 * Worker threads own only a job while its handler runs; the job returns through server->done_q.
 * One request is in flight per connection (reads are paused until its response is written), so
 * the parser arena is single-owner at any time.
 */
#include "server/http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stddef.h>
#include <sys/random.h>
#include <sys/socket.h>

#include "core/mpsc.h"
#include "core/threadpool.h"
#include "llhttp.h"
#include "server/auth.h"
#include "server/metrics.h"
#include "server/problem.h"
#include "server/ratelimit.h"
#include "server/router.h"
#include "version.h"

#define READ_BUF_SIZE    (64U * 1024U)
#define MAX_HEADER_VALUE 4096U
#define DEFAULT_MAX_BODY (32U * 1024U * 1024U)

/* ====================================================================== */
/* ULID and request ids                                                    */
/* ====================================================================== */

void av_ulid_generate(char out[27])
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t) ts.tv_sec * 1000U + (uint64_t) ts.tv_nsec / 1000000U;
    for (int i = 9; i >= 0; i--) {
        out[i] = alphabet[ms & 31U];
        ms >>= 5;
    }
    uint8_t rnd[10];
    size_t got = 0;
    while (got < sizeof(rnd)) {
        ssize_t n = getrandom(rnd + got, sizeof(rnd) - got, 0);
        if (n > 0) {
            got += (size_t) n;
        } else {
            /* getrandom only fails on EINTR here; fall back to the clock so ids stay unique enough */
            rnd[got] = (uint8_t) (ts.tv_nsec >> (got % 8U));
            got++;
        }
    }
    /* 80 bits -> 16 symbols of 5 bits */
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 10;
    for (size_t i = 0; i < sizeof(rnd); i++) {
        acc = (acc << 8) | rnd[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = alphabet[(acc >> (bits - 5)) & 31U];
            bits -= 5;
        }
        acc &= (1U << bits) - 1U;
    }
    out[26] = '\0';
}

bool av_request_id_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        if (!(isalnum(c) != 0 || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* ====================================================================== */
/* Incremental parser                                                      */
/* ====================================================================== */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} abuf;

static bool abuf_append(abuf *b, const char *s, size_t n, size_t limit)
{
    if (n > limit || b->len > limit - n) {
        return false;
    }
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap == 0 ? 256 : b->cap;
        while (nc < b->len + n + 1) {
            nc *= 2;
        }
        char *np = realloc(b->p, nc);
        if (np == NULL) {
            return false;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return true;
}

static void abuf_free(abuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

typedef enum { H_CONTENT_TYPE = 0, H_AUTHORIZATION, H_IDEMPOTENCY, H_REQUEST_ID, H_EXPECT, H_COUNT } hkind;

static const char *const k_hnames[H_COUNT] = {"content-type", "authorization", "idempotency-key",
                                              "x-request-id", "expect"};

struct av_httpp {
    llhttp_t parser;
    llhttp_settings_t settings;
    size_t max_body;
    av_arena *arena;

    bool started;
    bool complete;
    bool errored;
    bool want_continue;
    bool keep_alive;

    abuf url;
    abuf hv[H_COUNT];
    abuf body;
    size_t header_bytes;
    unsigned n_headers;
    char fname[40];
    size_t fname_len;
    bool fname_overflow;
    bool last_was_value;
    int cur_kind;

    av_http_req req;
    int err_status;
    av_err err;
};

static int fail(av_httpp *p, int status, const char *code, const char *msg)
{
    if (!p->errored) {
        p->errored = true;
        p->err_status = status;
        av_err_set(&p->err, code, "%s", msg);
    }
    return -1;
}

static int header_budget(av_httpp *p, size_t n)
{
    p->header_bytes += n;
    if (p->header_bytes > AV_HTTP_MAX_HEADER_BYTES) {
        return fail(p, 400, "bad_request", "request headers are too large");
    }
    return 0;
}

static av_httpp *self(llhttp_t *l)
{
    return (av_httpp *) l->data;
}

static int cb_message_begin(llhttp_t *l)
{
    self(l)->started = true;
    return 0;
}

static int cb_url(llhttp_t *l, const char *at, size_t n)
{
    av_httpp *p = self(l);
    if (header_budget(p, n) != 0) {
        return -1;
    }
    if (!abuf_append(&p->url, at, n, AV_HTTP_MAX_HEADER_BYTES)) {
        return fail(p, 400, "bad_request", "request target is too large");
    }
    return 0;
}

static void begin_header(av_httpp *p)
{
    p->fname_len = 0;
    p->fname_overflow = false;
    p->last_was_value = false;
    p->cur_kind = -1;
    p->n_headers++;
}

static int cb_header_field(llhttp_t *l, const char *at, size_t n)
{
    av_httpp *p = self(l);
    if (header_budget(p, n) != 0) {
        return -1;
    }
    if (p->last_was_value || p->n_headers == 0) {
        begin_header(p);
        if (p->n_headers > AV_HTTP_MAX_HEADERS) {
            return fail(p, 400, "bad_request", "too many request headers");
        }
    }
    if (p->fname_len + n >= sizeof(p->fname)) {
        p->fname_overflow = true;
    } else {
        memcpy(p->fname + p->fname_len, at, n);
        p->fname_len += n;
    }
    return 0;
}

static int cb_header_value(llhttp_t *l, const char *at, size_t n)
{
    av_httpp *p = self(l);
    if (header_budget(p, n) != 0) {
        return -1;
    }
    if (!p->last_was_value) {
        p->last_was_value = true;
        p->cur_kind = -1;
        if (!p->fname_overflow) {
            for (int k = 0; k < (int) H_COUNT; k++) {
                if (strlen(k_hnames[k]) == p->fname_len &&
                    strncasecmp(p->fname, k_hnames[k], p->fname_len) == 0) {
                    p->cur_kind = k;
                    p->hv[k].len = 0; /* last occurrence wins */
                    if (p->hv[k].p != NULL) {
                        p->hv[k].p[0] = '\0';
                    }
                    break;
                }
            }
        }
    }
    if (p->cur_kind >= 0 && !abuf_append(&p->hv[p->cur_kind], at, n, MAX_HEADER_VALUE)) {
        return fail(p, 400, "bad_request", "request header value is too large");
    }
    return 0;
}

static int cb_headers_complete(llhttp_t *l)
{
    av_httpp *p = self(l);
    if (llhttp_get_http_major(l) != 1) {
        return fail(p, 400, "bad_request", "only HTTP/1.x is supported");
    }
    if (llhttp_get_upgrade(l) != 0) {
        return fail(p, 400, "bad_request", "protocol upgrade is not supported");
    }
    if (l->content_length > p->max_body && (l->flags & F_CHUNKED) == 0) {
        return fail(p, 413, "payload_too_large", "request body exceeds the configured limit");
    }
    if (p->hv[H_EXPECT].len > 0 && strcasecmp(p->hv[H_EXPECT].p, "100-continue") == 0) {
        p->want_continue = true;
    }
    return 0;
}

static int cb_body(llhttp_t *l, const char *at, size_t n)
{
    av_httpp *p = self(l);
    if (!abuf_append(&p->body, at, n, p->max_body)) {
        if (p->body.len + n > p->max_body) {
            return fail(p, 413, "payload_too_large", "request body exceeds the configured limit");
        }
        return fail(p, 500, "internal", "out of memory buffering the request body");
    }
    return 0;
}

static int cb_message_complete(llhttp_t *l)
{
    av_httpp *p = self(l);
    p->complete = true;
    p->keep_alive = llhttp_should_keep_alive(l) != 0;
    return HPE_PAUSED;
}

av_httpp *av_httpp_create(size_t max_body)
{
    av_httpp *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->arena = av_arena_create("http-req", 16 * 1024, 0);
    if (p->arena == NULL) {
        free(p);
        return NULL;
    }
    p->max_body = max_body != 0 ? max_body : DEFAULT_MAX_BODY;
    llhttp_settings_init(&p->settings);
    p->settings.on_message_begin = cb_message_begin;
    p->settings.on_url = cb_url;
    p->settings.on_header_field = cb_header_field;
    p->settings.on_header_value = cb_header_value;
    p->settings.on_headers_complete = cb_headers_complete;
    p->settings.on_body = cb_body;
    p->settings.on_message_complete = cb_message_complete;
    llhttp_init(&p->parser, HTTP_REQUEST, &p->settings);
    p->parser.data = p;
    p->cur_kind = -1;
    return p;
}

void av_httpp_destroy(av_httpp *p)
{
    if (p == NULL) {
        return;
    }
    abuf_free(&p->url);
    for (int i = 0; i < (int) H_COUNT; i++) {
        abuf_free(&p->hv[i]);
    }
    abuf_free(&p->body);
    av_arena_destroy(p->arena);
    free(p);
}

static const char *hval(av_httpp *p, hkind k)
{
    return p->hv[k].len > 0 ? av_arena_strndup(p->arena, p->hv[k].p, p->hv[k].len) : NULL;
}

static bool build_request(av_httpp *p)
{
    av_arena *a = p->arena;
    const char *target = p->url.len > 0 ? p->url.p : "/";
    /* absolute-form: http://host/path */
    if (strncasecmp(target, "http://", 7) == 0 || strncasecmp(target, "https://", 8) == 0) {
        size_t skip = strncasecmp(target, "http://", 7) == 0 ? 7 : 8;
        const char *slash = strchr(target + skip, '/');
        target = slash != NULL ? slash : "/";
    }
    const char *hash = strchr(target, '#');
    size_t tlen = hash != NULL ? (size_t) (hash - target) : strlen(target);
    const char *q = memchr(target, '?', tlen);
    size_t plen = q != NULL ? (size_t) (q - target) : tlen;
    p->req.method = av_arena_strdup(a, llhttp_method_name((llhttp_method_t) llhttp_get_method(&p->parser)));
    p->req.path = av_arena_strndup(a, target, plen);
    p->req.query = q != NULL ? av_arena_strndup(a, q + 1, tlen - plen - 1) : "";
    p->req.body = p->body.p != NULL ? p->body.p : "";
    p->req.body_len = p->body.len;
    p->req.content_type = hval(p, H_CONTENT_TYPE);
    p->req.authorization = hval(p, H_AUTHORIZATION);
    p->req.idempotency_key = hval(p, H_IDEMPOTENCY);
    char rid[27];
    const char *client_rid = hval(p, H_REQUEST_ID);
    if (client_rid != NULL && av_request_id_valid(client_rid)) {
        p->req.request_id = client_rid;
    } else {
        av_ulid_generate(rid);
        p->req.request_id = av_arena_strdup(a, rid);
    }
    p->req.arena = a;
    return p->req.method != NULL && p->req.path != NULL && p->req.query != NULL && p->req.request_id != NULL;
}

av_httpp_result av_httpp_feed(av_httpp *p, const char *data, size_t len, size_t *consumed)
{
    *consumed = 0;
    if (p->errored) {
        return AV_HTTPP_ERROR;
    }
    if (p->complete) {
        return AV_HTTPP_REQUEST;
    }
    if (len == 0) {
        return AV_HTTPP_NEED_MORE;
    }
    llhttp_errno_t rc = llhttp_execute(&p->parser, data, len);
    if (rc == HPE_OK) {
        *consumed = len;
        return AV_HTTPP_NEED_MORE;
    }
    if (rc == HPE_PAUSED && p->complete) {
        const char *pos = llhttp_get_error_pos(&p->parser);
        *consumed = pos != NULL && pos >= data && pos <= data + len ? (size_t) (pos - data) : len;
        if (!build_request(p)) {
            fail(p, 500, "internal", "out of memory building the request");
            return AV_HTTPP_ERROR;
        }
        return AV_HTTPP_REQUEST;
    }
    if (!p->errored) {
        const char *reason = llhttp_get_error_reason(&p->parser);
        if (rc == HPE_PAUSED_UPGRADE || rc == HPE_PAUSED_H2_UPGRADE) {
            reason = "protocol upgrade is not supported";
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "malformed HTTP request: %s", reason != NULL ? reason : "parse error");
        fail(p, 400, "bad_request", msg);
    }
    return AV_HTTPP_ERROR;
}

const av_http_req *av_httpp_request(const av_httpp *p)
{
    return &p->req;
}

bool av_httpp_keep_alive(const av_httpp *p)
{
    return p->keep_alive;
}

bool av_httpp_take_continue(av_httpp *p)
{
    bool v = p->want_continue;
    p->want_continue = false;
    return v;
}

bool av_httpp_in_progress(const av_httpp *p)
{
    return p->started && !p->complete;
}

int av_httpp_error(const av_httpp *p, av_err *e)
{
    if (e != NULL) {
        *e = p->err;
    }
    return p->err_status;
}

void av_httpp_next(av_httpp *p)
{
    abuf_free(&p->url);
    for (int i = 0; i < (int) H_COUNT; i++) {
        abuf_free(&p->hv[i]);
    }
    abuf_free(&p->body);
    av_arena_reset(p->arena);
    memset(&p->req, 0, sizeof(p->req));
    p->started = false;
    p->complete = false;
    p->want_continue = false;
    p->keep_alive = false;
    p->header_bytes = 0;
    p->n_headers = 0;
    p->fname_len = 0;
    p->last_was_value = false;
    p->cur_kind = -1;
    llhttp_reset(&p->parser);
}

/* ====================================================================== */
/* Server                                                                  */
/* ====================================================================== */

typedef struct conn conn;

typedef struct job {
    av_mpsc_node node; /* first */
    conn *c;
    av_http_handler handler;
    void *hctx;
    const char *params[AV_ROUTER_MAX_PARAMS];
    size_t n_params;
    const av_http_req *req;
    av_http_resp resp;
    const char *route; /* metric label */
    uint64_t t0_ns;
    bool force_close;
    bool counted_in_flight;
    char rid[27];
    char method[16];
    char path[96];
} job;

struct conn {
    uv_tcp_t tcp; /* first */
    uv_timer_t timer;
    av_http_server *srv;
    av_httpp *parser;
    conn *prev;
    conn *next;
    char *stash;
    size_t stash_len;
    int open_handles; /* tcp + timer until their close callbacks ran */
    int pending;      /* jobs on the pool + writes in flight */
    bool closing;
    bool reading;
    bool busy; /* a request is being handled or its response written */
    char peer[64];
};

struct av_http_server {
    uv_loop_t *loop;
    av_http_config cfg;
    av_threadpool *pool;
    av_mpsc_wake done_q;
    uv_tcp_t listener;
    uv_timer_t drain_timer;
    bool drain_timer_init;
    int live_handles;
    bool listening;
    bool draining;
    bool final_closing;
    bool shutdown_requested;
    bool finished;
    size_t n_conns;
    conn *conns;
    char *rbuf;
    int port;
    void (*done)(void *);
    void *done_ctx;
    av_mfamily *m_requests;
    av_mfamily *m_duration;
    av_mfamily *m_in_flight;
    av_mfamily *m_conns;
    av_auth *auth;
    av_ratelimit *ratelimit;
};

static void conn_close(conn *c);
static void conn_after_response(conn *c);
static void read_cb(uv_stream_t *st, ssize_t nread, const uv_buf_t *buf);
static void alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf);

static const char *reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 415:
        return "Unsupported Media Type";
    case 422:
        return "Unprocessable Entity";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Status";
    }
}

static void server_maybe_finish(av_http_server *s);

static void handle_closed(uv_handle_t *h)
{
    av_http_server *s = (av_http_server *) h->data;
    s->live_handles--;
    server_maybe_finish(s);
}

static void wake_closed(uv_handle_t *h)
{
    /* the async handle's data points at the av_mpsc_wake embedded in the server */
    av_mpsc_wake *w = (av_mpsc_wake *) h->data;
    av_http_server *s = (av_http_server *) ((char *) w - offsetof(av_http_server, done_q));
    s->live_handles--;
    server_maybe_finish(s);
}

static void server_maybe_finish(av_http_server *s)
{
    if (!s->shutdown_requested) {
        return;
    }
    if (s->n_conns == 0 && !s->final_closing) {
        s->final_closing = true;
        if (s->drain_timer_init) {
            uv_timer_stop(&s->drain_timer);
            uv_close((uv_handle_t *) &s->drain_timer, handle_closed);
        }
        av_mpsc_wake_close(&s->done_q, wake_closed);
    }
    if (s->final_closing && s->live_handles == 0 && !s->finished) {
        s->finished = true;
        if (s->done != NULL) {
            s->done(s->done_ctx);
        }
    }
}

/* ---- connections ---- */

static void conn_free(conn *c)
{
    av_http_server *s = c->srv;
    if (c->prev != NULL) {
        c->prev->next = c->next;
    } else {
        s->conns = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }
    s->n_conns--;
    av_metric_set(s->m_conns, NULL, (double) s->n_conns);
    av_httpp_destroy(c->parser);
    free(c->stash);
    free(c);
    server_maybe_finish(s);
}

static void conn_handle_closed(uv_handle_t *h)
{
    conn *c = (conn *) h->data;
    c->open_handles--;
    if (c->open_handles == 0 && c->pending == 0) {
        conn_free(c);
    }
}

static void conn_close(conn *c)
{
    if (c->closing) {
        return;
    }
    c->closing = true;
    if (c->reading) {
        uv_read_stop((uv_stream_t *) &c->tcp);
        c->reading = false;
    }
    uv_timer_stop(&c->timer);
    uv_close((uv_handle_t *) &c->tcp, conn_handle_closed);
    uv_close((uv_handle_t *) &c->timer, conn_handle_closed);
}

static void conn_release(conn *c)
{
    c->pending--;
    if (c->closing && c->open_handles == 0 && c->pending == 0) {
        conn_free(c);
    }
}

static void on_timeout(uv_timer_t *t)
{
    conn *c = (conn *) t->data;
    av_log(AV_LOG_DEBUG, "http", "closing connection: %s timeout",
           av_httpp_in_progress(c->parser) ? "request receive" : "idle");
    conn_close(c);
}

static void arm_timer(conn *c, int ms)
{
    uv_timer_start(&c->timer, on_timeout, (uint64_t) ms, 0);
}

static void conn_start_reading(conn *c)
{
    if (!c->reading && !c->closing) {
        if (uv_read_start((uv_stream_t *) &c->tcp, alloc_cb, read_cb) == 0) {
            c->reading = true;
        }
    }
}

/* ---- response ---- */

typedef struct {
    uv_write_t req;
    job *j;
    char *hdr;
    bool close_after;
} write_ctx;

static void job_free(job *j)
{
    free(j->resp.body);
    free(j);
}

static void write_done(uv_write_t *w, int status)
{
    write_ctx *wc = (write_ctx *) w->data;
    conn *c = wc->j->c;
    bool close_after = wc->close_after || status != 0;
    free(wc->hdr);
    job_free(wc->j);
    free(wc);
    if (status != 0) {
        av_log(AV_LOG_DEBUG, "http", "write failed: %s", uv_strerror(status));
    }
    if (!c->closing) {
        if (close_after) {
            conn_close(c);
        } else {
            conn_after_response(c);
        }
    }
    conn_release(c);
}

static void record_metrics(av_http_server *s, job *j, int status)
{
    char st[8];
    snprintf(st, sizeof(st), "%d", status);
    const char *lv[2] = {j->route, st};
    av_metric_inc(s->m_requests, lv, 1);
    av_metric_observe(s->m_duration, lv, (double) (uv_hrtime() - j->t0_ns) / 1e9);
    if (j->counted_in_flight) {
        j->counted_in_flight = false;
        av_metric_inc(s->m_in_flight, NULL, -1);
    }
}

/* Loop thread: serialize and write. Takes ownership of j. */
static void send_job(job *j)
{
    conn *c = j->c;
    av_http_server *s = c->srv;
    av_http_resp *r = &j->resp;
    if (r->status == 0 || (r->body == NULL && r->body_len > 0)) {
        av_err e;
        av_err_set(&e, "internal", "handler produced no response");
        free(r->body);
        r->body = NULL;
        r->body_len = 0;
        av_problem_from_err(&e, r, NULL);
    }
    int status = r->status;
    bool keep = !j->force_close && !s->draining && av_httpp_keep_alive(c->parser);
    record_metrics(s, j, status);
    av_log(AV_LOG_DEBUG, "http", "%s %s -> %d (%.2f ms) rid=%s", j->method, j->path, status,
           (double) (uv_hrtime() - j->t0_ns) / 1e6, r->request_id != NULL ? r->request_id : "");

    char extra[256];
    size_t xo = 0;
    if (r->has_as_of) {
        xo += (size_t) snprintf(extra + xo, sizeof(extra) - xo, "Arkivector-As-Of-Block: %llu\r\n",
                                (unsigned long long) r->as_of_block);
    }
    if (r->retry_after != NULL && xo < sizeof(extra)) {
        xo += (size_t) snprintf(extra + xo, sizeof(extra) - xo, "Retry-After: %.16s\r\n", r->retry_after);
    }
    if (r->allow != NULL && xo < sizeof(extra)) {
        xo += (size_t) snprintf(extra + xo, sizeof(extra) - xo, "Allow: %.96s\r\n", r->allow);
    }
    if (xo >= sizeof(extra)) {
        xo = sizeof(extra) - 1;
    }
    extra[xo] = '\0';

    write_ctx *wc = calloc(1, sizeof(*wc));
    char *hdr = malloc(768);
    if (wc == NULL || hdr == NULL) {
        free(wc);
        free(hdr);
        job_free(j);
        conn_close(c);
        return;
    }
    int hn = snprintf(hdr, 768,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nX-Request-Id: %s\r\n"
                      "Arkivector-Api-Version: %s\r\n%sConnection: %s\r\n\r\n",
                      status, reason_phrase(status),
                      r->content_type != NULL ? r->content_type : "application/json", r->body_len,
                      r->request_id != NULL ? r->request_id : "-", AV_API_VERSION, extra,
                      keep ? "keep-alive" : "close");
    if (hn < 0 || hn >= 768) {
        free(wc);
        free(hdr);
        job_free(j);
        conn_close(c);
        return;
    }
    wc->j = j;
    wc->hdr = hdr;
    wc->close_after = !keep;
    wc->req.data = wc;
    uv_buf_t bufs[2] = {uv_buf_init(hdr, (unsigned) hn), uv_buf_init(r->body, (unsigned) r->body_len)};
    unsigned nb = r->body_len > 0 ? 2U : 1U;
    c->pending++;
    int rc = uv_write(&wc->req, (uv_stream_t *) &c->tcp, bufs, nb, write_done);
    if (rc != 0) {
        av_log(AV_LOG_DEBUG, "http", "uv_write: %s", uv_strerror(rc));
        free(hdr);
        free(wc);
        job_free(j);
        conn_release(c);
        conn_close(c);
    }
}

static job *job_new(conn *c, const char *rid)
{
    job *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        return NULL;
    }
    j->c = c;
    j->t0_ns = uv_hrtime();
    j->route = "unmatched";
    snprintf(j->rid, sizeof(j->rid), "%s", rid);
    j->resp.request_id = j->rid;
    return j;
}

/* ---- dispatch ---- */

static bool is_json_content_type(const char *ct)
{
    if (ct == NULL) {
        return false;
    }
    while (*ct == ' ' || *ct == '\t') {
        ct++;
    }
    static const char json[] = "application/json";
    size_t n = sizeof(json) - 1;
    if (strncasecmp(ct, json, n) != 0) {
        return false;
    }
    ct += n;
    while (*ct == ' ' || *ct == '\t') {
        ct++;
    }
    return *ct == '\0' || *ct == ';';
}

static void worker_run(void *arg)
{
    job *j = (job *) arg;
    j->handler(j->req, j->params, j->n_params, &j->resp, j->hctx);
    av_mpsc_wake_push(&j->c->srv->done_q, &j->node);
}

static void done_cb(av_mpsc_wake *w, void *ctx)
{
    (void) ctx;
    av_mpsc_node *n;
    while ((n = av_mpsc_pop(&w->q)) != NULL) {
        job *j = (job *) n;
        conn *c = j->c;
        if (c->closing) {
            record_metrics(c->srv, j, j->resp.status != 0 ? j->resp.status : 500);
            job_free(j);
            conn_release(c);
            continue;
        }
        send_job(j);
        conn_release(c); /* the pool's pending reference; the write took its own */
    }
}

static void respond_problem(job *j, const char *code, int status_override, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void respond_problem(job *j, const char *code, int status_override, const char *fmt, ...)
{
    av_err e;
    va_list ap;
    va_start(ap, fmt);
    av_err_clear(&e);
    snprintf(e.code, sizeof(e.code), "%s", code);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);
    av_problem_from_err(&e, &j->resp, NULL);
    if (status_override != 0) {
        /* 405: same problem type as route_not_found, status 405 (patch the status member). */
        char *needle = strstr(j->resp.body, "\"status\":404");
        if (needle != NULL) {
            memcpy(needle + 9, "405", 3);
        }
        j->resp.status = status_override;
    }
    send_job(j);
}

static void dispatch(conn *c)
{
    av_http_server *s = c->srv;
    const av_http_req *req = av_httpp_request(c->parser);
    c->busy = true;
    uv_timer_stop(&c->timer);
    uv_read_stop((uv_stream_t *) &c->tcp);
    c->reading = false;

    job *j = job_new(c, req->request_id);
    if (j == NULL) {
        conn_close(c);
        return;
    }
    snprintf(j->method, sizeof(j->method), "%s", req->method);
    snprintf(j->path, sizeof(j->path), "%s", req->path);
    j->req = req;
    j->counted_in_flight = true;
    av_metric_inc(s->m_in_flight, NULL, 1);

    av_route_match m;
    av_router_match(s->cfg.router, req->method, req->path, req->arena, &m);
    j->route = m.status == 0 ? m.pattern : "unmatched";
    if (m.bad_encoding) {
        respond_problem(j, "bad_request", 0, "invalid percent-encoding in the request path");
        return;
    }
    if (m.status == 404) {
        respond_problem(j, "route_not_found", 0, "no route for %s %s", req->method, req->path);
        return;
    }
    if (m.status == 405) {
        char *allow = av_arena_strdup(req->arena, m.allow);
        j->resp.allow = allow;
        respond_problem(j, "route_not_found", 405, "method %s is not allowed for %s (allowed: %s)",
                        req->method, req->path, m.allow);
        return;
    }
    if (req->body_len > 0 && !is_json_content_type(req->content_type)) {
        respond_problem(j, "unsupported_media_type", 0, "request bodies must be application/json");
        return;
    }

    if (s->auth != NULL && av_auth_is_enabled(s->auth) && !av_auth_is_unauthenticated_path(req->path)) {
        uint32_t req_scope = av_auth_required_scope(req->method, req->path);
        const char *key_id = NULL;
        av_err ae;
        av_err_clear(&ae);
        if (av_auth_check(s->auth, req->authorization, req_scope, &key_id, &ae) != AV_OK) {
            int st = strcmp(ae.code, "unauthorized") == 0 ? 401 : 403;
            respond_problem(j, ae.code, st, "%s", ae.msg);
            return;
        }
    }

    if (s->ratelimit != NULL && av_ratelimit_is_enabled(s->ratelimit)) {
        const char *id = req->authorization != NULL ? req->authorization : (c->peer[0] != '\0' ? c->peer : "127.0.0.1");
        int retry_after = 0;
        av_err re;
        av_err_clear(&re);
        if (av_ratelimit_check(s->ratelimit, id, &retry_after, &re) != AV_OK) {
            char ra[16];
            snprintf(ra, sizeof(ra), "%d", retry_after);
            j->resp.retry_after = av_arena_strdup(req->arena, ra);
            respond_problem(j, "rate_limited", 429, "%s", re.msg);
            return;
        }
    }

    j->handler = m.handler;
    j->hctx = m.ctx;
    j->n_params = m.n_params;
    for (size_t i = 0; i < m.n_params; i++) {
        j->params[i] = m.params[i];
    }
    c->pending++; /* the job on the pool */
    av_err e;
    if (av_threadpool_submit(s->pool, worker_run, j, &e) != AV_OK) {
        c->pending--;
        respond_problem(j, "not_ready", 0, "server is overloaded, retry shortly");
    }
}

/* ---- reading ---- */

static void alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
    (void) suggested;
    conn *c = (conn *) h->data;
    *buf = uv_buf_init(c->srv->rbuf, READ_BUF_SIZE);
}

static void process(conn *c, const char *data, size_t len)
{
    av_http_server *s = c->srv;
    size_t off = 0;
    while (off < len && !c->closing) {
        size_t used = 0;
        av_httpp_result res = av_httpp_feed(c->parser, data + off, len - off, &used);
        off += used;
        if (av_httpp_take_continue(c->parser)) {
            static const char k_continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
            uv_buf_t b = uv_buf_init((char *) (uintptr_t) k_continue, sizeof(k_continue) - 1);
            (void) uv_try_write((uv_stream_t *) &c->tcp, &b, 1);
        }
        if (res == AV_HTTPP_NEED_MORE) {
            continue;
        }
        c->busy = true;
        if (res == AV_HTTPP_ERROR) {
            av_err e;
            int status = av_httpp_error(c->parser, &e);
            char rid[27];
            av_ulid_generate(rid);
            uv_read_stop((uv_stream_t *) &c->tcp);
            c->reading = false;
            uv_timer_stop(&c->timer);
            job *j = job_new(c, rid);
            if (j == NULL) {
                conn_close(c);
                return;
            }
            j->force_close = true;
            snprintf(j->method, sizeof(j->method), "-");
            av_problem_from_err(&e, &j->resp, NULL);
            j->resp.status = status;
            av_metric_inc(s->m_in_flight, NULL, 1);
            j->counted_in_flight = true;
            send_job(j);
            return;
        }
        /* a complete request: keep the pipelined remainder for after the response */
        if (off < len) {
            c->stash = malloc(len - off);
            if (c->stash == NULL) {
                conn_close(c);
                return;
            }
            memcpy(c->stash, data + off, len - off);
            c->stash_len = len - off;
        }
        dispatch(c);
        return;
    }
}

static void read_cb(uv_stream_t *st, ssize_t nread, const uv_buf_t *buf)
{
    conn *c = (conn *) st->data;
    if (nread < 0) {
        if (nread != UV_EOF) {
            av_log(AV_LOG_DEBUG, "http", "read error: %s", uv_strerror((int) nread));
        }
        conn_close(c);
        return;
    }
    if (nread == 0 || c->closing) {
        return;
    }
    if (!av_httpp_in_progress(c->parser)) {
        arm_timer(c, c->srv->cfg.request_timeout_ms); /* first byte of a request: slow-loris clock starts */
    }
    process(c, buf->base, (size_t) nread);
}

/* Called after a response was fully written on a keep-alive connection. */
static void conn_after_response(conn *c)
{
    av_http_server *s = c->srv;
    c->busy = false;
    av_httpp_next(c->parser);
    if (s->draining) {
        conn_close(c);
        return;
    }
    arm_timer(c, s->cfg.idle_timeout_ms);
    if (c->stash != NULL) {
        char *buf = c->stash;
        size_t n = c->stash_len;
        c->stash = NULL;
        c->stash_len = 0;
        arm_timer(c, s->cfg.request_timeout_ms);
        process(c, buf, n);
        free(buf);
        if (!c->closing && !c->busy) {
            conn_start_reading(c);
        }
        return;
    }
    conn_start_reading(c);
}

/* ---- accept ---- */

static void on_connection(uv_stream_t *listener, int status)
{
    av_http_server *s = (av_http_server *) listener->data;
    if (status != 0 || s->draining) {
        return;
    }
    conn *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return;
    }
    c->srv = s;
    c->parser = av_httpp_create(s->cfg.max_body);
    if (c->parser == NULL || uv_tcp_init(s->loop, &c->tcp) != 0) {
        av_httpp_destroy(c->parser);
        free(c);
        return;
    }
    c->tcp.data = c;
    uv_timer_init(s->loop, &c->timer); /* cannot fail */
    c->timer.data = c;
    c->open_handles = 2;
    c->next = s->conns;
    if (s->conns != NULL) {
        s->conns->prev = c;
    }
    s->conns = c;
    s->n_conns++;
    av_metric_set(s->m_conns, NULL, (double) s->n_conns);
    if (uv_accept(listener, (uv_stream_t *) &c->tcp) != 0 || s->n_conns > s->cfg.max_connections) {
        if (s->n_conns > s->cfg.max_connections) {
            av_log(AV_LOG_WARN, "http", "connection limit %zu reached, dropping a connection",
                   s->cfg.max_connections);
        }
        conn_close(c);
        return;
    }
    uv_tcp_nodelay(&c->tcp, 1);
    uv_tcp_keepalive(&c->tcp, 1, 60);
    struct sockaddr_storage ss;
    int sslen = sizeof(ss);
    if (uv_tcp_getpeername(&c->tcp, (struct sockaddr *) &ss, &sslen) == 0) {
        if (ss.ss_family == AF_INET) {
            uv_ip4_name((const struct sockaddr_in *) &ss, c->peer, sizeof(c->peer));
        } else if (ss.ss_family == AF_INET6) {
            uv_ip6_name((const struct sockaddr_in6 *) &ss, c->peer, sizeof(c->peer));
        }
    }
    arm_timer(c, s->cfg.idle_timeout_ms);
    if (uv_read_start((uv_stream_t *) &c->tcp, alloc_cb, read_cb) == 0) {
        c->reading = true;
    } else {
        conn_close(c);
    }
}

/* ---- lifecycle ---- */

av_http_server *av_http_create(uv_loop_t *loop, const av_http_config *cfg, av_err *e)
{
    if (cfg == NULL || cfg->router == NULL || cfg->listen == NULL) {
        av_err_set(e, "internal", "http: router and listen address are required");
        return NULL;
    }
    av_http_server *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        av_err_set(e, "internal", "http: out of memory");
        return NULL;
    }
    s->loop = loop;
    s->cfg = *cfg;
    if (s->cfg.max_body == 0) {
        s->cfg.max_body = DEFAULT_MAX_BODY;
    }
    if (s->cfg.workers == 0) {
        s->cfg.workers = 4;
    }
    if (s->cfg.max_queue == 0) {
        s->cfg.max_queue = 4096;
    }
    if (s->cfg.max_connections == 0) {
        s->cfg.max_connections = 1024;
    }
    if (s->cfg.request_timeout_ms == 0) {
        s->cfg.request_timeout_ms = 30000;
    }
    if (s->cfg.idle_timeout_ms == 0) {
        s->cfg.idle_timeout_ms = 60000;
    }
    if (s->cfg.drain_timeout_ms == 0) {
        s->cfg.drain_timeout_ms = 10000;
    }
    s->rbuf = malloc(READ_BUF_SIZE);
    s->pool = av_threadpool_create("query", s->cfg.workers, s->cfg.max_queue);
    if (s->rbuf == NULL || s->pool == NULL) {
        av_err_set(e, "internal", "http: cannot create the query pool");
        goto fail;
    }
    if (av_mpsc_wake_init(&s->done_q, loop, done_cb, s, e) != AV_OK) {
        goto fail;
    }
    s->live_handles = 1;
    if (uv_tcp_init(loop, &s->listener) != 0) {
        av_err_set(e, "internal", "http: uv_tcp_init failed");
        av_mpsc_wake_close(&s->done_q, NULL);
        goto fail;
    }
    s->listener.data = s;
    s->live_handles++;
    static const char *const rl[] = {"route", "status"};
    s->m_requests = av_metric_register("arkivector_http_requests_total", "HTTP requests handled.",
                                       AV_MTYPE_COUNTER, rl, 2);
    s->m_duration = av_metric_register("arkivector_http_request_duration_seconds", "HTTP request latency.",
                                       AV_MTYPE_HISTOGRAM, rl, 2);
    s->m_in_flight = av_metric_register("arkivector_http_in_flight_requests", "Requests being handled.",
                                        AV_MTYPE_GAUGE, NULL, 0);
    s->m_conns =
        av_metric_register("arkivector_http_connections", "Open HTTP connections.", AV_MTYPE_GAUGE, NULL, 0);

    if (s->cfg.api_keys_file != NULL && s->cfg.api_keys_file[0] != '\0') {
        s->auth = av_auth_create(s->cfg.api_keys_file, e);
        if (s->auth == NULL) {
            goto fail;
        }
    }
    if (s->cfg.ratelimit_rps > 0.0) {
        s->ratelimit = av_ratelimit_create(s->cfg.ratelimit_rps, s->cfg.ratelimit_burst);
    }

    return s;
fail:
    if (s->auth != NULL) {
        av_auth_destroy(s->auth);
    }
    if (s->ratelimit != NULL) {
        av_ratelimit_destroy(s->ratelimit);
    }
    av_threadpool_destroy(s->pool);
    free(s->rbuf);
    free(s);
    return NULL;
}

static av_status parse_listen(const char *spec, struct sockaddr_storage *out, av_err *e)
{
    const char *colon = strrchr(spec, ':');
    if (colon == NULL || colon == spec) {
        av_err_set(e, "invalid_argument", "listen address '%s' is not host:port", spec);
        return AV_ERR;
    }
    char host[128];
    size_t hl = (size_t) (colon - spec);
    if (hl >= sizeof(host)) {
        av_err_set(e, "invalid_argument", "listen host is too long");
        return AV_ERR;
    }
    memcpy(host, spec, hl);
    host[hl] = '\0';
    if (host[0] == '[' && hl >= 2 && host[hl - 1] == ']') {
        memmove(host, host + 1, hl - 2);
        host[hl - 2] = '\0';
    }
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (*end != '\0' || colon[1] == '\0' || port < 0 || port > 65535) {
        av_err_set(e, "invalid_argument", "listen address '%s' has an invalid port", spec);
        return AV_ERR;
    }
    if (strcmp(host, "localhost") == 0) {
        snprintf(host, sizeof(host), "127.0.0.1");
    }
    struct sockaddr_in a4;
    struct sockaddr_in6 a6;
    if (uv_ip4_addr(host, (int) port, &a4) == 0) {
        memcpy(out, &a4, sizeof(a4));
        return AV_OK;
    }
    if (uv_ip6_addr(host, (int) port, &a6) == 0) {
        memcpy(out, &a6, sizeof(a6));
        return AV_OK;
    }
    av_err_set(e, "invalid_argument", "listen host '%s' is not an IP address", host);
    return AV_ERR;
}

av_status av_http_listen(av_http_server *s, av_err *e)
{
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    if (parse_listen(s->cfg.listen, &addr, e) != AV_OK) {
        return AV_ERR;
    }
    int rc = uv_tcp_bind(&s->listener, (const struct sockaddr *) &addr, 0);
    if (rc != 0) {
        av_err_set(e, "internal", "cannot bind %s: %s", s->cfg.listen, uv_strerror(rc));
        return AV_ERR;
    }
    rc = uv_listen((uv_stream_t *) &s->listener, 511, on_connection);
    if (rc != 0) {
        av_err_set(e, "internal", "cannot listen on %s: %s", s->cfg.listen, uv_strerror(rc));
        return AV_ERR;
    }
    struct sockaddr_storage bound;
    int blen = (int) sizeof(bound);
    if (uv_tcp_getsockname(&s->listener, (struct sockaddr *) &bound, &blen) == 0) {
        s->port = bound.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *) &bound)->sin6_port)
                                              : ntohs(((struct sockaddr_in *) &bound)->sin_port);
    }
    s->listening = true;
    return AV_OK;
}

int av_http_port(const av_http_server *s)
{
    return s->listening ? s->port : 0;
}

static void drain_expired(uv_timer_t *t)
{
    av_http_server *s = (av_http_server *) t->data;
    av_log(AV_LOG_WARN, "http", "drain timeout: closing %zu connection(s) with unfinished requests",
           s->n_conns);
    for (conn *c = s->conns; c != NULL; c = c->next) {
        conn_close(c);
    }
}

void av_http_shutdown(av_http_server *s, void (*done)(void *ctx), void *ctx)
{
    if (s->shutdown_requested) {
        return;
    }
    s->shutdown_requested = true;
    s->draining = true;
    s->done = done;
    s->done_ctx = ctx;
    uv_close((uv_handle_t *) &s->listener, handle_closed);
    s->listening = false;
    uv_timer_init(s->loop, &s->drain_timer);
    s->drain_timer.data = s;
    s->drain_timer_init = true;
    s->live_handles++;
    uv_timer_start(&s->drain_timer, drain_expired, (uint64_t) s->cfg.drain_timeout_ms, 0);
    for (conn *c = s->conns; c != NULL;) {
        conn *next = c->next; /* conn_close does not unlink, but keep the walk robust */
        if (!c->busy) {
            conn_close(c);
        }
        c = next;
    }
    server_maybe_finish(s);
}

void av_http_destroy(av_http_server *s)
{
    if (s == NULL) {
        return;
    }
    if (s->auth != NULL) {
        av_auth_destroy(s->auth);
    }
    if (s->ratelimit != NULL) {
        av_ratelimit_destroy(s->ratelimit);
    }
    av_threadpool_destroy(s->pool);
    free(s->rbuf);
    free(s);
}
