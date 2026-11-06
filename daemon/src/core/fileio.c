/*
 * core/fileio.c: bounded whole-file reads (see fileio.h).
 */
#include "core/fileio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *av_read_file(const char *path, const char *what, size_t max_bytes, size_t *out_len, av_err *e)
{
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        av_err_set(e, "invalid_argument", "%s: cannot open %s: %s", what, path, strerror(errno));
        return NULL;
    }
    /* Read at most max_bytes + 1 bytes so an over-long file is detected without trusting ftell. */
    size_t limit = max_bytes + 1;
    size_t cap = limit < 4096 ? limit + 1 : 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        av_err_set(e, "internal", "%s: out of memory reading %s", what, path);
        return NULL;
    }
    for (;;) {
        if (len + 1 >= cap) {
            if (cap > limit) {
                break; /* read limit + 1 bytes: too large, reported below */
            }
            size_t ncap = cap * 2 > limit + 1 ? limit + 1 : cap * 2;
            char *nb = realloc(buf, ncap);
            if (nb == NULL) {
                free(buf);
                fclose(f);
                av_err_set(e, "internal", "%s: out of memory reading %s", what, path);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        size_t want = cap - len - 1;
        size_t n = fread(buf + len, 1, want, f);
        if (n > want) {
            n = want;
        }
        len += n;
        if (n < want) {
            break; /* EOF or error */
        }
    }
    int bad = ferror(f);
    fclose(f);
    if (bad != 0) {
        free(buf);
        av_err_set(e, "invalid_argument", "%s: cannot read %s", what, path);
        return NULL;
    }
    if (len > max_bytes) {
        free(buf);
        av_err_set(e, "invalid_argument", "%s: %s is larger than %zu bytes", what, path, max_bytes);
        return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}
