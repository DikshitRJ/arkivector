/*
 * port.h (pgcompat stub): portability helpers referenced by copied code
 * (pgvector includes it "for random()" and "for strtof()"). ARKIVECTOR-owned.
 */
#ifndef PG_PORT_H
#define PG_PORT_H

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* PostgreSQL wraps these for portability; on POSIX the libc versions are fine. */
#define pg_strcasecmp  strcasecmp
#define pg_strncasecmp strncasecmp
#define pg_qsort       qsort
#define qsort_arg      av_pg_qsort_arg

typedef int (*qsort_arg_comparator)(const void *a, const void *b, void *arg);
void av_pg_qsort_arg(void *base, size_t nel, size_t elsize, qsort_arg_comparator cmp, void *arg);

static inline unsigned char pg_tolower(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        ch = (unsigned char) (ch + ('a' - 'A'));
    }
    return ch;
}

static inline unsigned char pg_toupper(unsigned char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (unsigned char) (ch - ('a' - 'A'));
    }
    return ch;
}

/* pg_strong_random is only used by pg_prng_strong_seed, which Arkivector never calls. */
bool pg_strong_random(void *buf, size_t len);

#endif /* PG_PORT_H */
