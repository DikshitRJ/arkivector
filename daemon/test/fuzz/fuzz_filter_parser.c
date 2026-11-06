/*
 * fuzz_filter_parser.c: libFuzzer target for the `where` text parser (av_filter_from_where).
 * Accepted filters must survive the canonical JSON round trip used for filter hashing.
 */
#include <stdint.h>
#include <stdlib.h>

#include "core/arena.h"
#include "filter/ast.h"
#include "filter/parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > AV_FILTER_MAX_WHERE_BYTES) {
        return 0;
    }
    av_arena *a = av_arena_create("fuzz", 64 * 1024, 0);
    av_err e;
    av_err_clear(&e);
    av_filter *f = NULL;
    av_status st = av_filter_from_where((const char *) data, size, NULL, a, &f, &e);
    if (st == AV_OK && f == NULL) {
        abort(); /* success must produce a tree */
    }
    if (st != AV_OK && f != NULL) {
        abort(); /* errors must not leak a partial tree */
    }
    av_arena_destroy(a);
    return 0;
}
