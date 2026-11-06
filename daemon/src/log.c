/*
 * log.c: av_err helpers and the stderr logger (levels, text/JSON, secret redaction).
 */
#include "log.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static _Atomic int g_level = AV_LOG_INFO;
static _Atomic int g_format = AV_LOG_TEXT;

void av_err_clear(av_err *e)
{
    if (e == NULL) {
        return;
    }
    memset(e, 0, sizeof(*e));
    e->position = -1;
}

static void set_code(av_err *e, const char *code)
{
    size_t n = strlen(code);
    if (n >= sizeof(e->code)) {
        n = sizeof(e->code) - 1;
    }
    memcpy(e->code, code, n);
    e->code[n] = '\0';
}

void av_err_set(av_err *e, const char *code, const char *fmt, ...)
{
    if (e == NULL) {
        return;
    }
    av_err_clear(e);
    set_code(e, code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

void av_err_set_vb(av_err *e, const char *vb_code, const char *fmt, ...)
{
    if (e == NULL) {
        return;
    }
    av_err_clear(e);
    set_code(e, "invalid_vector");
    snprintf(e->vb_error, sizeof(e->vb_error), "%s", vb_code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

void av_err_field(av_err *e, const char *field)
{
    if (e != NULL) {
        snprintf(e->field, sizeof(e->field), "%s", field);
    }
}

void av_log_set_level(av_log_level level)
{
    atomic_store_explicit(&g_level, (int) level, memory_order_relaxed);
}

bool av_log_enabled(av_log_level level)
{
    return (int) level >= atomic_load_explicit(&g_level, memory_order_relaxed);
}

void av_log_set_format(av_log_format format)
{
    atomic_store_explicit(&g_format, (int) format, memory_order_relaxed);
}

bool av_log_parse_level(const char *name, av_log_level *out)
{
    static const char *const names[] = {"debug", "info", "warn", "error"};
    if (name == NULL) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (strcasecmp(name, names[i]) == 0) {
            *out = (av_log_level) i;
            return true;
        }
    }
    return false;
}

bool av_log_parse_format(const char *name, av_log_format *out)
{
    if (name == NULL) {
        return false;
    }
    if (strcasecmp(name, "text") == 0) {
        *out = AV_LOG_TEXT;
        return true;
    }
    if (strcasecmp(name, "json") == 0) {
        *out = AV_LOG_JSON;
        return true;
    }
    return false;
}

/* ---- redaction ---- */

static const char *const k_secret_names[] = {"x-api-key", "authorization", "access_key",
                                             "api_key",   "private_key",   "secret",
                                             "password",  "token",         "bearer"};

static bool is_ident(char c)
{
    return isalnum((unsigned char) c) != 0 || c == '_' || c == '-';
}

static bool is_value_end(char c)
{
    return c == '\0' || isspace((unsigned char) c) != 0 || c == '"' || c == '\'' || c == ',' || c == '&' ||
           c == ';' || c == '}' || c == ')';
}

/* Length of the secret name at s (case-insensitive, whole identifier), 0 if none. */
static size_t match_secret_name(const char *s)
{
    for (size_t i = 0; i < sizeof(k_secret_names) / sizeof(k_secret_names[0]); i++) {
        size_t n = strlen(k_secret_names[i]);
        if (strncasecmp(s, k_secret_names[i], n) == 0 && !is_ident(s[n])) {
            return n;
        }
    }
    return 0;
}

static void put(char *out, size_t cap, size_t *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n && *o + 1 < cap; i++) {
        out[(*o)++] = s[i];
    }
}

void av_log_redact(const char *in, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t o = 0;
    const char *p = in;
    while (*p != '\0' && o + 1 < cap) {
        bool boundary = (p == in) || !is_ident(p[-1]);
        size_t n = boundary ? match_secret_name(p) : 0;
        if (n == 0) {
            out[o++] = *p++;
            continue;
        }
        bool is_bearer = strncasecmp(p, "bearer", 6) == 0;
        const char *q = p + n;
        while (*q == ' ' || *q == '\t' || *q == ':' || *q == '=' || *q == '"' || *q == '\'') {
            q++;
        }
        if (q == p + n || is_value_end(*q)) {
            /* Not an assignment (no separator) or an empty value: keep the text. */
            put(out, cap, &o, p, n);
            p += n;
            continue;
        }
        const char *val = q;
        while (!is_value_end(*q)) {
            q++;
        }
        /* "Authorization: Bearer abc": the scheme word is not the secret, redact the next token. */
        size_t vlen = (size_t) (q - val);
        if (!is_bearer &&
            ((vlen == 6 && strncasecmp(val, "bearer", 6) == 0) ||
             (vlen == 5 && strncasecmp(val, "basic", 5) == 0)) &&
            *q == ' ') {
            while (*q == ' ') {
                q++;
            }
            if (!is_value_end(*q)) {
                const char *tok = q;
                while (!is_value_end(*q)) {
                    q++;
                }
                put(out, cap, &o, p, (size_t) (tok - p));
                put(out, cap, &o, "[REDACTED]", 10);
                p = q;
                continue;
            }
        }
        put(out, cap, &o, p, (size_t) (val - p));
        put(out, cap, &o, "[REDACTED]", 10);
        p = q;
    }
    out[o] = '\0';
}

/* ---- output ---- */

static void json_escape(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (; *s != '\0' && o + 7 < cap; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char) c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\';
            out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\';
            out[o++] = 't';
        } else if (c < 0x20) {
            o += (size_t) snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char) c;
        }
    }
    out[o] = '\0';
}

void av_vlog(av_log_level level, const char *module, const char *fmt, va_list ap)
{
    static const char *const names[] = {"debug", "info", "warn", "error"};
    if ((unsigned) level > 3U || !av_log_enabled(level)) {
        return;
    }
    char raw[2048];
    char line[2048];
    vsnprintf(raw, sizeof(raw), fmt, ap);
    av_log_redact(raw, line, sizeof(line));

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    time_t secs = ts.tv_sec;
    gmtime_r(&secs, &tmv);
    char when[48];
    size_t wn = strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(when + wn, sizeof(when) - wn, ".%03ldZ", ts.tv_nsec / 1000000L);

    char out[4608];
    int n;
    const char *mod = module != NULL ? module : "";
    if (atomic_load_explicit(&g_format, memory_order_relaxed) == AV_LOG_JSON) {
        char esc_msg[4096];
        char esc_mod[96];
        json_escape(esc_msg, sizeof(esc_msg), line);
        json_escape(esc_mod, sizeof(esc_mod), mod);
        n = snprintf(out, sizeof(out), "{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}\n",
                     when, names[level], esc_mod, esc_msg);
    } else {
        n = snprintf(out, sizeof(out), "ts=%s level=%s module=%s msg=\"%s\"\n", when, names[level], mod,
                     line);
    }
    if (n > 0) {
        size_t len = (size_t) n < sizeof(out) ? (size_t) n : sizeof(out) - 1;
        fwrite(out, 1, len, stderr);
    }
}

void av_log(av_log_level level, const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog(level, module, fmt, ap);
    va_end(ap);
}
