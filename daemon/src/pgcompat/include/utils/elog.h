/*
 * utils/elog.h (pgcompat stub): ereport/elog and PG_TRY/PG_CATCH/PG_END_TRY.
 *
 * Semantics follow PostgreSQL 17: ereport(ERROR, ...) does not return; it
 * siglongjmps to the innermost PG_TRY frame of the current thread. Levels below
 * ERROR are logged and execution continues. FATAL/PANIC abort the process.
 * The error data of the last caught error is available via av_pg_error_data().
 *
 * Before jumping, every LWLock held by the thread is released (PostgreSQL's
 * LWLockReleaseAll() on abort), see av_pg_on_error_release_locks in lwlock.c.
 *
 * ARKIVECTOR-owned. See STUBS.md.
 */
#ifndef ELOG_H
#define ELOG_H

#include <setjmp.h>

/* Error levels (PostgreSQL 17 values). */
#define DEBUG5 10
#define DEBUG4 11
#define DEBUG3 12
#define DEBUG2 13
#define DEBUG1 14
#define LOG 15
#define LOG_SERVER_ONLY 16
#define COMMERROR LOG_SERVER_ONLY
#define INFO 17
#define NOTICE 18
#define WARNING 19
#define PGWARNING 19
#define WARNING_CLIENT_ONLY 20
#define ERROR 21
#define PGERROR 21
#define FATAL 22
#define PANIC 23

/* SQLSTATE encoding (PostgreSQL MAKE_SQLSTATE). */
#define PGSIXBIT(ch) (((ch) - '0') & 0x3F)
#define PGUNSIXBIT(val) (((val) & 0x3F) + '0')
#define MAKE_SQLSTATE(ch1, ch2, ch3, ch4, ch5) \
    (PGSIXBIT(ch1) + (PGSIXBIT(ch2) << 6) + (PGSIXBIT(ch3) << 12) + (PGSIXBIT(ch4) << 18) + (PGSIXBIT(ch5) << 24))

#include "utils/errcodes.h"

/* ---- ereport ---- */
extern bool errstart(int elevel, const char *domain);
extern bool errstart_cold(int elevel, const char *domain) pg_attribute_cold;
extern void errfinish(const char *filename, int lineno, const char *funcname);

extern int errcode(int sqlerrcode);
extern int errmsg(const char *fmt, ...) pg_attribute_printf(1, 2);
extern int errmsg_internal(const char *fmt, ...) pg_attribute_printf(1, 2);
extern int errmsg_plural(const char *fmt_singular, const char *fmt_plural, unsigned long n, ...)
    pg_attribute_printf(1, 4) pg_attribute_printf(2, 4);
extern int errdetail(const char *fmt, ...) pg_attribute_printf(1, 2);
extern int errdetail_internal(const char *fmt, ...) pg_attribute_printf(1, 2);
extern int errhint(const char *fmt, ...) pg_attribute_printf(1, 2);
extern int errcontext_msg(const char *fmt, ...) pg_attribute_printf(1, 2);
extern int errposition(int cursorpos);
extern int errhidestmt(bool hide_stmt);
extern int errhidecontext(bool hide_ctx);

#define TEXTDOMAIN NULL

#define ereport_domain(elevel, domain, ...)                                 \
    do {                                                                    \
        const int elevel_ = (elevel);                                       \
        if (errstart(elevel_, domain))                                      \
            __VA_ARGS__, errfinish(__FILE__, __LINE__, __func__);           \
        if (elevel_ >= ERROR)                                               \
            pg_unreachable();                                               \
    } while (0)

#define ereport(elevel, ...) ereport_domain(elevel, TEXTDOMAIN, __VA_ARGS__)

#define elog(elevel, ...) ereport(elevel, errmsg_internal(__VA_ARGS__))

/* ---- PG_TRY ---- */
extern __thread sigjmp_buf *PG_exception_stack;

#define PG_TRY(...)                                                          \
    do {                                                                     \
        sigjmp_buf *_save_exception_stack##__VA_ARGS__ = PG_exception_stack; \
        sigjmp_buf _local_sigjmp_buf##__VA_ARGS__;                           \
        bool _do_rethrow##__VA_ARGS__ = false;                               \
        if (sigsetjmp(_local_sigjmp_buf##__VA_ARGS__, 0) == 0) {             \
            PG_exception_stack = &_local_sigjmp_buf##__VA_ARGS__

#define PG_CATCH(...)                                         \
    }                                                         \
    else                                                      \
    {                                                         \
        PG_exception_stack = _save_exception_stack##__VA_ARGS__;

#define PG_FINALLY(...)                                       \
    }                                                         \
    else _do_rethrow##__VA_ARGS__ = true;                     \
    {                                                         \
        PG_exception_stack = _save_exception_stack##__VA_ARGS__;

#define PG_END_TRY(...)                                          \
    }                                                            \
    if (_do_rethrow##__VA_ARGS__)                                \
        PG_RE_THROW();                                           \
    PG_exception_stack = _save_exception_stack##__VA_ARGS__;     \
    }                                                            \
    while (0)

#define PG_RE_THROW() pg_re_throw()
extern void pg_re_throw(void) pg_attribute_noreturn();

/* ---- error data ---- */
typedef struct ErrorData {
    int elevel;
    int sqlerrcode;
    const char *filename;
    int lineno;
    const char *funcname;
    char message[512];
    char detail[512];
    char hint[256];
    int cursorpos;
} ErrorData;

/* Data of the most recent ereport(ERROR) on this thread (valid inside PG_CATCH). */
extern const ErrorData *av_pg_error_data(void);
extern ErrorData *CopyErrorData(void);
extern void FlushErrorState(void);
extern char *unpack_sql_state(int sql_state);

/* Hook used by lwlock.c to release held locks before unwinding (set in D0). */
extern void (*av_pg_on_error_release_locks)(void);

/* Log sink for levels < ERROR (default: stderr). Set by the daemon's logger. */
typedef void (*av_pg_log_fn)(int elevel, const char *message);
extern void av_pg_set_log_hook(av_pg_log_fn fn);

/* Minimum level that gets logged (default WARNING). */
extern int log_min_messages;

#endif /* ELOG_H */
