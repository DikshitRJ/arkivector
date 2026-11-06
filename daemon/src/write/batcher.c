/*
 * write/batcher.c: packs mutation operations into transaction batches.
 */
#include "write/batcher.h"

#include <stdlib.h>
#include <string.h>

av_status av_batcher_pack(const av_mutation_op *ops, size_t n_ops, const av_batcher_config *cfg,
                          av_arena *arena, av_op_batch **out_batches, size_t *out_n_batches,
                          av_err *e)
{
    *out_batches = NULL;
    *out_n_batches = 0;
    if (n_ops == 0)
        return AV_OK;

    size_t max_bytes = (cfg != NULL && cfg->max_tx_bytes > 0) ? cfg->max_tx_bytes : 120000;
    size_t max_ops = (cfg != NULL && cfg->max_ops_per_tx > 0) ? cfg->max_ops_per_tx : 100;

    size_t batch_cap = (n_ops / max_ops) + 4;
    av_op_batch *batches = av_arena_alloc(arena, batch_cap * sizeof(av_op_batch), _Alignof(av_op_batch));
    if (batches == NULL) {
        av_err_set(e, "internal", "out of memory allocating batches");
        return AV_ERR;
    }

    size_t n_batches = 0;
    size_t start = 0;

    while (start < n_ops) {
        size_t count = 1;
        uint8_t *last_calldata = NULL;
        size_t last_len = 0;

        if (av_abi_encode_execute(ops + start, 1, arena, &last_calldata, &last_len, e) != AV_OK)
            return AV_ERR;

        while (start + count < n_ops && count < max_ops) {
            uint8_t *test_calldata = NULL;
            size_t test_len = 0;
            if (av_abi_encode_execute(ops + start, count + 1, arena, &test_calldata, &test_len, e) != AV_OK)
                return AV_ERR;

            if (test_len > max_bytes)
                break;

            last_calldata = test_calldata;
            last_len = test_len;
            count++;
        }

        if (n_batches >= batch_cap) {
            size_t ncap = batch_cap * 2;
            av_op_batch *nb = av_arena_alloc(arena, ncap * sizeof(av_op_batch), _Alignof(av_op_batch));
            if (nb == NULL) {
                av_err_set(e, "internal", "out of memory growing batches");
                return AV_ERR;
            }
            memcpy(nb, batches, n_batches * sizeof(av_op_batch));
            batches = nb;
            batch_cap = ncap;
        }

        batches[n_batches].ops = ops + start;
        batches[n_batches].n_ops = count;
        batches[n_batches].calldata = last_calldata;
        batches[n_batches].calldata_len = last_len;
        n_batches++;

        start += count;
    }

    *out_batches = batches;
    *out_n_batches = n_batches;
    return AV_OK;
}
