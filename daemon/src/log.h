/*
 * log.h: status codes, error objects (av_err) and leveled logging.
 *
 * av_err carries an error code from spec/error-codes.json ("invalid_vector", ...),
 * an optional VectorBytes sub-code (VB_*), the JSON field it refers to, a byte
 * position (filters) and the block for consistency errors. Logging (av_log) is
 * completed in D2: until then it writes text lines to stderr.
 */
#ifndef AV_LOG_H
#define AV_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { AV_OK = 0, AV_ERR = 1 } av_status;

typedef struct {
    char code[40];     /* spec/error-codes.json code, "" when unset */
    char vb_error[32]; /* VB_* code for invalid_vector, else "" */
    char field[128];   /* JSON pointer of the offending field, else "" */
    int position;      /* byte offset for invalid_filter, -1 when unset */
    uint64_t as_of_block;
    char msg[512];
} av_err;

/* Reset to "no error". NULL is allowed. */
void av_err_clear(av_err *e);

/* Set code and printf-style message. NULL e is allowed (the error is dropped). */
void av_err_set(av_err *e, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Set an invalid_vector error with a VB_* sub-code. */
void av_err_set_vb(av_err *e, const char *vb_code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Set the field (JSON pointer) on an existing error. */
void av_err_field(av_err *e, const char *field);

typedef enum { AV_LOG_DEBUG = 0, AV_LOG_INFO = 1, AV_LOG_WARN = 2, AV_LOG_ERROR = 3 } av_log_level;

void av_log(av_log_level level, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void av_vlog(av_log_level level, const char *module, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));
void av_log_set_level(av_log_level level);
bool av_log_enabled(av_log_level level);

#endif /* AV_LOG_H */
