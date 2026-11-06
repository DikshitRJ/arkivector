/*
 * fuzz_json_filter.c: libFuzzer target for the JSON filter AST parser (av_filter_from_json).
 * The input is parsed with yyjson first (as the HTTP layer does), then handed to the filter parser.
 */
#include <stdint.h>
#include <stdlib.h>

#include "core/arena.h"
#include "filter/json_filter.h"
#include "yyjson.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 256 * 1024) {
        return 0;
    }
    yyjson_doc *doc = yyjson_read((const char *) data, size, 0);
    if (doc == NULL) {
        return 0;
    }
    av_arena *a = av_arena_create("fuzz", 64 * 1024, 0);
    av_err e;
    av_err_clear(&e);
    av_filter *f = NULL;
    av_status st = av_filter_from_json(yyjson_doc_get_root(doc), NULL, a, &f, &e);
    if ((st == AV_OK) != (f != NULL)) {
        abort();
    }
    av_arena_destroy(a);
    yyjson_doc_free(doc);
    return 0;
}
