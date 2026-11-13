/*
 * server/router.c: segment router. See router.h.
 */
#include "server/router.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text; /* literal text, or param name */
    int is_param;
} seg;

typedef struct route {
    char *method;
    char *pattern;
    seg segs[AV_ROUTER_MAX_SEGMENTS];
    size_t n_segs;
    size_t n_literal;
    av_http_handler handler;
    void *ctx;
    struct route *next;
} route;

struct av_router {
    route *head;
    route *tail;
};

av_router *av_router_create(void)
{
    return calloc(1, sizeof(av_router));
}

void av_router_destroy(av_router *r)
{
    if (r == NULL) {
        return;
    }
    for (route *x = r->head; x != NULL;) {
        route *n = x->next;
        for (size_t i = 0; i < x->n_segs; i++) {
            free(x->segs[i].text);
        }
        free(x->method);
        free(x->pattern);
        free(x);
        x = n;
    }
    free(r);
}

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d != NULL) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

av_status av_router_add(av_router *r, const char *method, const char *pattern, av_http_handler h, void *ctx,
                        av_err *e)
{
    if (r == NULL || method == NULL || pattern == NULL || h == NULL || pattern[0] != '/') {
        av_err_set(e, "internal", "router: invalid route registration");
        return AV_ERR;
    }
    for (route *x = r->head; x != NULL; x = x->next) {
        if (strcmp(x->method, method) == 0 && strcmp(x->pattern, pattern) == 0) {
            av_err_set(e, "internal", "router: duplicate route %s %s", method, pattern);
            return AV_ERR;
        }
    }
    route *x = calloc(1, sizeof(*x));
    if (x == NULL) {
        av_err_set(e, "internal", "router: out of memory");
        return AV_ERR;
    }
    x->method = dup_n(method, strlen(method));
    x->pattern = dup_n(pattern, strlen(pattern));
    x->handler = h;
    x->ctx = ctx;
    size_t n_params = 0;
    const char *p = pattern + 1;
    for (;;) {
        const char *end = strchr(p, '/');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        if (len == 0 && !(end == NULL && p == pattern + 1)) {
            av_err_set(e, "internal", "router: empty segment in %s", pattern);
            goto fail;
        }
        if (x->n_segs == AV_ROUTER_MAX_SEGMENTS) {
            av_err_set(e, "internal", "router: too many segments in %s", pattern);
            goto fail;
        }
        seg *s = &x->segs[x->n_segs];
        if (len >= 2 && p[0] == '{' && p[len - 1] == '}') {
            s->is_param = 1;
            s->text = dup_n(p + 1, len - 2);
            n_params++;
        } else {
            if (memchr(p, '{', len) != NULL || memchr(p, '}', len) != NULL) {
                av_err_set(e, "internal", "router: braces must wrap a whole segment in %s", pattern);
                goto fail;
            }
            s->text = dup_n(p, len);
            x->n_literal++;
        }
        x->n_segs++;
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    if (n_params > AV_ROUTER_MAX_PARAMS) {
        av_err_set(e, "internal", "router: too many parameters in %s", pattern);
        goto fail;
    }
    if (r->tail != NULL) {
        r->tail->next = x;
    } else {
        r->head = x;
    }
    r->tail = x;
    return AV_OK;
fail:
    for (size_t i = 0; i < x->n_segs; i++) {
        free(x->segs[i].text);
    }
    free(x->method);
    free(x->pattern);
    free(x);
    return AV_ERR;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

char *av_percent_decode(av_arena *a, const char *s, size_t len)
{
    char *out = av_arena_alloc(a, len + 1, 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%') {
            if (i + 2 >= len) {
                return NULL;
            }
            int hi = hexv(s[i + 1]);
            int lo = hexv(s[i + 2]);
            if (hi < 0 || lo < 0) {
                return NULL;
            }
            int v = hi * 16 + lo;
            if (v == 0) {
                return NULL;
            }
            out[o++] = (char) v;
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return out;
}

/* Try to match one route against the path segments. Returns 1 on match. */
static int try_match(const route *x, const char *const *sv, const size_t *sl, size_t ns)
{
    if (x->n_segs != ns) {
        return 0;
    }
    for (size_t i = 0; i < ns; i++) {
        const seg *s = &x->segs[i];
        if (s->is_param) {
            if (sl[i] == 0) {
                return 0;
            }
        } else if (strlen(s->text) != sl[i] || memcmp(s->text, sv[i], sl[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static void add_allow(av_route_match *m, const char *method)
{
    size_t cur = strlen(m->allow);
    const char *p = m->allow;
    size_t ml = strlen(method);
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t n = comma != NULL ? (size_t) (comma - p) : strlen(p);
        if (n == ml && memcmp(p, method, ml) == 0) {
            return;
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
        while (*p == ' ') {
            p++;
        }
    }
    if (cur + ml + 3 >= sizeof(m->allow)) {
        return;
    }
    if (cur > 0) {
        memcpy(m->allow + cur, ", ", 2);
        cur += 2;
    }
    memcpy(m->allow + cur, method, ml + 1);
}

void av_router_match(const av_router *r, const char *method, const char *path, av_arena *a, av_route_match *m)
{
    memset(m, 0, sizeof(*m));
    m->pattern = "";
    m->status = 404;
    if (r == NULL || path == NULL || path[0] != '/') {
        return;
    }
    const char *sv[AV_ROUTER_MAX_SEGMENTS];
    size_t sl[AV_ROUTER_MAX_SEGMENTS];
    size_t ns = 0;
    const char *p = path + 1;
    for (;;) {
        const char *end = strchr(p, '/');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        if (ns == AV_ROUTER_MAX_SEGMENTS) {
            return;
        }
        if (len == 0 && end == NULL && ns > 0) {
            break; /* one trailing slash ("/qdrant/") matches the route without it */
        }
        sv[ns] = p;
        sl[ns] = len;
        ns++;
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    const route *best = NULL;
    for (const route *x = r->head; x != NULL; x = x->next) {
        if (!try_match(x, sv, sl, ns)) {
            continue;
        }
        if (strcmp(x->method, method) == 0) {
            if (best == NULL || x->n_literal > best->n_literal) {
                best = x;
            }
        } else {
            add_allow(m, x->method);
        }
    }
    if (best == NULL) {
        if (m->allow[0] != '\0') {
            m->status = 405;
        }
        return;
    }
    m->status = 0;
    m->handler = best->handler;
    m->ctx = best->ctx;
    m->pattern = best->pattern;
    m->allow[0] = '\0';
    for (size_t i = 0; i < best->n_segs; i++) {
        if (!best->segs[i].is_param) {
            continue;
        }
        char *v = av_percent_decode(a, sv[i], sl[i]);
        if (v == NULL) {
            m->bad_encoding = 1;
            return;
        }
        m->params[m->n_params++] = v;
    }
}
