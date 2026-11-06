/*
 * fuzz_vector_in.c: libFuzzer target for the pgvector text parsers (vector_in,
 * halfvec_in, sparsevec_in, PostgreSQL bit_in) through av_text_to_pgvec.
 * Byte 0 selects the type; the rest is the NUL-terminated literal. Accepted inputs
 * must format with *_out and re-parse to a byte-identical struct.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec/text.h"
#include "core/arena.h"
#include "kernels/dispatch.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 256 * 1024) {
        return 0;
    }
    av_kernels_init();
    av_vtype t = (av_vtype) (1 + data[0] % 4);
    char *s = malloc(size);
    memcpy(s, data + 1, size - 1);
    s[size - 1] = '\0';
    av_arena *a = av_arena_create("fuzz", 64 * 1024, 0);
    av_err e;
    void *v = NULL;
    if (av_text_to_pgvec(t, s, -1, a, &v, &e) == AV_OK) {
        char *txt = NULL;
        if (av_pgvec_to_text(t, v, a, &txt, &e) != AV_OK) {
            abort();
        }
        void *v2 = NULL;
        if (av_text_to_pgvec(t, txt, -1, a, &v2, &e) != AV_OK) {
            abort();
        }
        /* halfvec/vector text output is shortest round-trip, so the structs must match */
        size_t n = av_pgvec_size(v);
        if (n != av_pgvec_size(v2) || memcmp(v, v2, n) != 0) {
            abort();
        }
    }
    av_arena_destroy(a);
    free(s);
    return 0;
}
