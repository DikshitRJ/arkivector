/*
 * fuzz_abi.c: libFuzzer target for the execute((uint8,bytes)[]) calldata decoder and the
 * revert decoder. Decoded op lists must re-encode and decode to the same op count.
 */
#include <stdint.h>
#include <stdlib.h>

#include "arkiv/abi.h"
#include "core/arena.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 512 * 1024) {
        return 0;
    }
    (void) av_abi_decode_revert(data, size);
    (void) av_abi_revert_error_name(data, size);

    av_arena *a = av_arena_create("fuzz", 64 * 1024, 0);
    av_err e;
    av_err_clear(&e);
    av_mutation_op *ops = NULL;
    size_t n = 0;
    if (av_abi_decode_execute(data, size, a, &ops, &n, &e) == AV_OK) {
        uint8_t *cd = NULL;
        size_t cd_len = 0;
        if (av_abi_encode_execute(ops, n, a, &cd, &cd_len, &e) == AV_OK) {
            av_mutation_op *ops2 = NULL;
            size_t n2 = 0;
            if (av_abi_decode_execute(cd, cd_len, a, &ops2, &n2, &e) != AV_OK || n2 != n) {
                abort();
            }
        }
    }
    av_arena_destroy(a);
    return 0;
}
