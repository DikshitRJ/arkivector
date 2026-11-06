/*
 * pgcompat/src/misc.c: small port.h helpers (qsort_arg, pg_strong_random).
 * ARKIVECTOR-owned.
 */
#define _GNU_SOURCE 1
#include "postgres.h"

#include <fcntl.h>

#include "utils/builtins.h"
#include "utils/float.h"
#include <unistd.h>

/* qsort with a context argument: glibc's qsort_r has the same comparator shape. */
void av_pg_qsort_arg(void *base, size_t nel, size_t elsize, qsort_arg_comparator cmp, void *arg)
{
    qsort_r(base, nel, elsize, cmp, arg);
}

bool pg_strong_random(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            close(fd);
            return false;
        }
        p += n;
        len -= (size_t) n;
    }
    close(fd);
    return true;
}

/* Only reachable through list_copy_deep()/list_member() on Node lists, unused by pgvector. */
void *copyObjectImpl(const void *from)
{
    (void) from;
    elog(ERROR, "copyObject is not supported in pgcompat");
    return NULL;
}

bool equal(const void *a, const void *b)
{
    (void) a;
    (void) b;
    elog(ERROR, "equal() on nodes is not supported in pgcompat");
    return false;
}

int pg_ltoa(int32 value, char *a)
{
    return snprintf(a, 12, "%d", (int) value);
}

/* Bodies from PostgreSQL 17 src/backend/utils/adt/float.c. */
void float_overflow_error(void)
{
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: overflow")));
    pg_unreachable();
}

void float_underflow_error(void)
{
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: underflow")));
    pg_unreachable();
}

void float_zero_divide_error(void)
{
    ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
    pg_unreachable();
}
