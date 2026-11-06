/*
 * fuzz_vectorbytes.c: libFuzzer target for the VectorBytes parser.
 *
 * Property: validate/decode never crash or read out of bounds on arbitrary bytes, and
 * every accepted blob re-encodes to identical bytes (flags included).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "codec/vectorbytes.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    av_err e;
    av_err_clear(&e);
    av_vb_info vi;
    av_status vs = av_vb_validate(data, size, &vi, &e);

    av_arena *a = av_arena_create("fuzz-vb", 4096, 0);
    if (a == NULL)
        abort();
    void *pg = NULL;
    av_vb_info di;
    av_status ds = av_vb_decode(data, size, a, &pg, &di, &e);
    if ((vs == AV_OK) != (ds == AV_OK))
        abort();
    if (ds == AV_OK) {
        if (vi.type != di.type || vi.dim != di.dim || vi.nnz != di.nnz || vi.flags != di.flags)
            abort();
        uint8_t *out = NULL;
        size_t outn = 0;
        if (av_vb_encode(pg, di.type, (di.flags & AV_VB_FLAG_NORMALIZED) != 0, a, &out, &outn, &e) != AV_OK)
            abort();
        if (outn != size || memcmp(out, data, size) != 0)
            abort();
    }
    av_arena_destroy(a);
    return 0;
}
