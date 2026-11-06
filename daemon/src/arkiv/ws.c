/*
 * arkiv/ws.c: see ws.h.
 */
#include "arkiv/ws.h"

#include <curl/curl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arkiv/entity.h"
#include "yyjson.h"

#define AV_WS_BACKOFF_MIN_MS 200
#define AV_WS_BACKOFF_MAX_MS 5000
#define AV_WS_MSG_MAX        ((size_t) 1024 * 1024)
#define AV_WS_SUBSCRIBE \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_subscribe\",\"params\":[\"newHeads\"]}"

struct av_ws {
    char *url;
    char *access_key;
    CURL *curl; /* NULL when disconnected */
    uint64_t next_try_ms;
    uint64_t backoff_ms;
    char *msg; /* partial message accumulator */
    size_t msg_len;
    size_t msg_cap;
};

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000U + (uint64_t) ts.tv_nsec / 1000000U;
}

static void disconnect(av_ws *w)
{
    if (w->curl != NULL) {
        curl_easy_cleanup(w->curl);
        w->curl = NULL;
    }
    w->msg_len = 0;
}

/* Wait for the socket; returns >0 ready, 0 timeout, <0 error. */
static int wait_socket(CURL *c, short events, int timeout_ms)
{
    curl_socket_t s = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &s) != CURLE_OK || s == CURL_SOCKET_BAD)
        return -1;
    struct pollfd p = {.fd = s, .events = events, .revents = 0};
    return poll(&p, 1, timeout_ms);
}

static bool send_text(CURL *c, const char *msg)
{
    size_t total = strlen(msg);
    size_t off = 0;
    for (int spins = 0; off < total && spins < 100; spins++) {
        size_t sent = 0;
        CURLcode cc = curl_ws_send(c, msg + off, total - off, &sent, 0, CURLWS_TEXT);
        off += sent;
        if (cc == CURLE_AGAIN) {
            if (wait_socket(c, POLLOUT, 1000) <= 0)
                return false;
        } else if (cc != CURLE_OK) {
            return false;
        }
    }
    return off >= total;
}

static bool connect_now(av_ws *w)
{
    CURL *c = curl_easy_init();
    struct curl_slist *hdrs = NULL;
    if (c == NULL)
        return false;
    if (w->access_key != NULL) {
        size_t n = strlen(w->access_key) + 16;
        char *h = malloc(n);
        if (h == NULL) {
            curl_easy_cleanup(c);
            return false;
        }
        snprintf(h, n, "X-API-KEY: %s", w->access_key);
        hdrs = curl_slist_append(NULL, h);
        free(h);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    }
    curl_easy_setopt(c, CURLOPT_URL, w->url);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode cc = curl_easy_perform(c);
    /* libcurl copies the header list into the handle at perform time only for the handshake. */
    if (cc == CURLE_OK && !send_text(c, AV_WS_SUBSCRIBE))
        cc = CURLE_SEND_ERROR;
    curl_slist_free_all(hdrs);
    if (cc != CURLE_OK) {
        curl_easy_cleanup(c);
        return false;
    }
    w->curl = c;
    w->msg_len = 0;
    return true;
}

av_ws *av_ws_open(const char *url, const char *access_key, av_err *e)
{
    const char *rest = NULL;
    const char *scheme = NULL;
    if (url != NULL) {
        if (strncmp(url, "ws://", 5) == 0 || strncmp(url, "http://", 7) == 0) {
            scheme = "ws://";
            rest = url + (url[0] == 'w' ? 5 : 7);
        } else if (strncmp(url, "wss://", 6) == 0 || strncmp(url, "https://", 8) == 0) {
            scheme = "wss://";
            rest = url + (url[0] == 'w' ? 6 : 8);
        }
    }
    if (rest == NULL || rest[0] == '\0') {
        av_err_set(e, "invalid_argument", "ws: url must start with ws://, wss://, http:// or https://");
        return NULL;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { /* idempotent; refcounted */
        av_err_set(e, "internal", "ws: curl_global_init failed");
        return NULL;
    }
    av_ws *w = calloc(1, sizeof *w);
    if (w == NULL) {
        av_err_set(e, "internal", "ws: out of memory");
        return NULL;
    }
    size_t n = strlen(scheme) + strlen(rest) + 1;
    w->url = malloc(n);
    if (w->url != NULL)
        snprintf(w->url, n, "%s%s", scheme, rest);
    if (access_key != NULL && access_key[0] != '\0')
        w->access_key = strdup(access_key);
    if (w->url == NULL || (access_key != NULL && access_key[0] != '\0' && w->access_key == NULL)) {
        av_ws_close(w);
        av_err_set(e, "internal", "ws: out of memory");
        return NULL;
    }
    w->backoff_ms = AV_WS_BACKOFF_MIN_MS;
    if (!connect_now(w)) {
        av_log(AV_LOG_INFO, "arkiv", "ws: initial connect failed, will retry with backoff");
        w->next_try_ms = mono_ms() + w->backoff_ms;
    }
    return w;
}

void av_ws_close(av_ws *w)
{
    if (w == NULL)
        return;
    disconnect(w);
    curl_global_cleanup();
    free(w->url);
    free(w->access_key);
    free(w->msg);
    free(w);
}

int av_ws_connected(const av_ws *w)
{
    return w != NULL && w->curl != NULL;
}

/* Extract a head number from a complete text message. */
static bool message_head(const char *s, size_t len, uint64_t *head)
{
    yyjson_doc *d = yyjson_read(s, len, 0);
    bool ok = false;
    if (d == NULL)
        return false;
    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_val *m = yyjson_obj_get(root, "method");
    if (yyjson_is_str(m) && strcmp(yyjson_get_str(m), "eth_subscription") == 0) {
        yyjson_val *num = yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "params"), "result"), "number");
        const char *ns = yyjson_get_str(num);
        ok = ns != NULL && strlen(ns) == yyjson_get_len(num) && av_qty_parse(ns, head);
    }
    yyjson_doc_free(d);
    return ok;
}

int av_ws_wait(av_ws *w, int timeout_ms, uint64_t *head)
{
    uint64_t deadline = mono_ms() + (uint64_t) (timeout_ms > 0 ? timeout_ms : 0);

    if (w->curl == NULL) {
        if (mono_ms() < w->next_try_ms)
            return -1;
        if (!connect_now(w)) {
            w->backoff_ms =
                w->backoff_ms * 2 > AV_WS_BACKOFF_MAX_MS ? AV_WS_BACKOFF_MAX_MS : w->backoff_ms * 2;
            w->next_try_ms = mono_ms() + w->backoff_ms;
            return -1;
        }
        w->backoff_ms = AV_WS_BACKOFF_MIN_MS;
    }
    for (;;) {
        char buf[8192];
        size_t nread = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode cc = curl_ws_recv(w->curl, buf, sizeof buf, &nread, &meta);
        if (cc == CURLE_AGAIN) {
            uint64_t now = mono_ms();
            if (now >= deadline)
                return 0;
            int pr = wait_socket(w->curl, POLLIN, (int) (deadline - now));
            if (pr == 0)
                return 0;
            if (pr < 0) {
                disconnect(w);
                w->next_try_ms = mono_ms() + w->backoff_ms;
                return -1;
            }
            continue;
        }
        if (cc != CURLE_OK || meta == NULL || (meta->flags & CURLWS_CLOSE) != 0) {
            av_log(AV_LOG_INFO, "arkiv", "ws: disconnected");
            disconnect(w);
            w->next_try_ms = mono_ms() + w->backoff_ms;
            return -1;
        }
        if ((meta->flags & CURLWS_TEXT) == 0 && w->msg_len == 0)
            continue; /* ping/pong/binary: libcurl answers pings itself */
        if (w->msg_len + nread > AV_WS_MSG_MAX) {
            disconnect(w);
            w->next_try_ms = mono_ms() + w->backoff_ms;
            return -1;
        }
        if (w->msg_len + nread + 1 > w->msg_cap) {
            size_t nc = w->msg_cap == 0 ? 4096 : w->msg_cap;
            while (nc < w->msg_len + nread + 1)
                nc *= 2;
            char *np = realloc(w->msg, nc);
            if (np == NULL) {
                disconnect(w);
                return -1;
            }
            w->msg = np;
            w->msg_cap = nc;
        }
        memcpy(w->msg + w->msg_len, buf, nread);
        w->msg_len += nread;
        if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0) {
            uint64_t h = 0;
            bool got = message_head(w->msg, w->msg_len, &h);
            w->msg_len = 0;
            if (got) {
                *head = h;
                return 1;
            }
        }
    }
}
