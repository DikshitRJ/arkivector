/*
 * fuzz_rawtx.c: libFuzzer target for the signed EIP-1559 transaction decoder (arkiv/rawtx.c), which
 * parses client-signed transactions (entities:submit) and remote-signer answers. A decoded
 * transaction must keep its data inside the input buffer.
 */
#include <stdint.h>
#include <stdlib.h>

#include "arkiv/rawtx.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    av_rawtx rt;
    av_err e;
    av_err_clear(&e);
    if (av_rawtx_decode(data, size, &rt, &e) == AV_OK) {
        if (rt.data_len > size || rt.data < data || rt.data + rt.data_len > data + size || rt.y_parity > 1) {
            abort();
        }
    }
    return 0;
}
