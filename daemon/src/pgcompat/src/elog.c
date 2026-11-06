/*
 * pgcompat/src/elog.c: ereport/elog and PG_TRY unwinding. See utils/elog.h.
 * ARKIVECTOR-owned.
 */
#include "postgres.h"

#include "storage/lwlock.h"

__thread sigjmp_buf *PG_exception_stack = NULL;

void (*av_pg_on_error_release_locks)(void) = NULL;
int log_min_messages = WARNING;

static av_pg_log_fn log_hook = NULL;

/* One in-flight report per thread (ereport is not reentrant in copied code). */
static __thread ErrorData cur_error;
static __thread ErrorData last_error;
static __thread int recursion_depth = 0;

void av_pg_set_log_hook(av_pg_log_fn fn)
{
    log_hook = fn;
}

bool errstart(int elevel, const char *domain)
{
    (void) domain;
    if (elevel < ERROR && elevel < log_min_messages) {
        return false;
    }
    if (recursion_depth > 0) {
        /* error while formatting an error: give up loudly */
        fprintf(stderr, "pgcompat: recursive ereport (level %d)\n", elevel);
        abort();
    }
    recursion_depth++;
    memset(&cur_error, 0, sizeof(cur_error));
    cur_error.elevel = elevel;
    cur_error.sqlerrcode = (elevel >= ERROR) ? ERRCODE_INTERNAL_ERROR : ERRCODE_SUCCESSFUL_COMPLETION;
    return true;
}

bool errstart_cold(int elevel, const char *domain)
{
    return errstart(elevel, domain);
}

int errcode(int sqlerrcode)
{
    cur_error.sqlerrcode = sqlerrcode;
    return 0;
}

static void vfmt(char *dst, size_t n, const char *fmt, va_list ap)
{
    vsnprintf(dst, n, fmt, ap);
}

int errmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(cur_error.message, sizeof(cur_error.message), fmt, ap);
    va_end(ap);
    return 0;
}

int errmsg_internal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(cur_error.message, sizeof(cur_error.message), fmt, ap);
    va_end(ap);
    return 0;
}

int errmsg_plural(const char *fmt_singular, const char *fmt_plural, unsigned long n, ...)
{
    va_list ap;
    va_start(ap, n);
    vfmt(cur_error.message, sizeof(cur_error.message), n == 1 ? fmt_singular : fmt_plural, ap);
    va_end(ap);
    return 0;
}

int errdetail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(cur_error.detail, sizeof(cur_error.detail), fmt, ap);
    va_end(ap);
    return 0;
}

int errdetail_internal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(cur_error.detail, sizeof(cur_error.detail), fmt, ap);
    va_end(ap);
    return 0;
}

int errhint(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfmt(cur_error.hint, sizeof(cur_error.hint), fmt, ap);
    va_end(ap);
    return 0;
}

int errcontext_msg(const char *fmt, ...)
{
    (void) fmt;
    return 0;
}

int errposition(int cursorpos)
{
    cur_error.cursorpos = cursorpos;
    return 0;
}

int errhidestmt(bool hide_stmt)
{
    (void) hide_stmt;
    return 0;
}

int errhidecontext(bool hide_ctx)
{
    (void) hide_ctx;
    return 0;
}

static const char *level_name(int elevel)
{
    switch (elevel) {
    case DEBUG5:
    case DEBUG4:
    case DEBUG3:
    case DEBUG2:
    case DEBUG1:
        return "DEBUG";
    case LOG:
    case LOG_SERVER_ONLY:
        return "LOG";
    case INFO:
        return "INFO";
    case NOTICE:
        return "NOTICE";
    case WARNING:
    case WARNING_CLIENT_ONLY:
        return "WARNING";
    case ERROR:
        return "ERROR";
    case FATAL:
        return "FATAL";
    default:
        return "PANIC";
    }
}

void errfinish(const char *filename, int lineno, const char *funcname)
{
    cur_error.filename = filename;
    cur_error.lineno = lineno;
    cur_error.funcname = funcname;
    recursion_depth--;

    int elevel = cur_error.elevel;
    if (elevel < ERROR) {
        if (log_hook != NULL) {
            log_hook(elevel, cur_error.message);
        } else {
            fprintf(stderr, "%s: %s\n", level_name(elevel), cur_error.message);
        }
        return;
    }
    if (elevel >= FATAL) {
        fprintf(stderr, "%s: %s (%s:%d)\n", level_name(elevel), cur_error.message, filename, lineno);
        abort();
    }

    last_error = cur_error;
    pg_re_throw();
}

void pg_re_throw(void)
{
    /* PostgreSQL's AbortTransaction calls LWLockReleaseAll(); do the same before unwinding. */
    LWLockReleaseAll();
    if (av_pg_on_error_release_locks != NULL) {
        av_pg_on_error_release_locks();
    }
    if (PG_exception_stack != NULL) {
        siglongjmp(*PG_exception_stack, 1);
    }
    fprintf(stderr, "pgcompat: ERROR outside PG_TRY: %s (%s:%d)\n", last_error.message,
            last_error.filename ? last_error.filename : "?", last_error.lineno);
    abort();
}

const ErrorData *av_pg_error_data(void)
{
    return &last_error;
}

ErrorData *CopyErrorData(void)
{
    ErrorData *e = palloc(sizeof(ErrorData));
    *e = last_error;
    return e;
}

void FlushErrorState(void)
{
    memset(&last_error, 0, sizeof(last_error));
    recursion_depth = 0;
}

char *unpack_sql_state(int sql_state)
{
    static __thread char buf[12];
    for (int i = 0; i < 5; i++) {
        buf[i] = (char) PGUNSIXBIT(sql_state);
        sql_state >>= 6;
    }
    buf[5] = '\0';
    return buf;
}
