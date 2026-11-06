/*
 * log.c: av_err helpers and a minimal stderr logger (completed in D2).
 */
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static av_log_level g_level = AV_LOG_INFO;

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
    g_level = level;
}

bool av_log_enabled(av_log_level level)
{
    return level >= g_level;
}

void av_vlog(av_log_level level, const char *module, const char *fmt, va_list ap)
{
    static const char *names[] = {"debug", "info", "warn", "error"};
    if (!av_log_enabled(level)) {
        return;
    }
    char line[2048];
    vsnprintf(line, sizeof(line), fmt, ap);
    fprintf(stderr, "level=%s module=%s msg=\"%s\"\n", names[level], module, line);
}

void av_log(av_log_level level, const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_vlog(level, module, fmt, ap);
    va_end(ap);
}
