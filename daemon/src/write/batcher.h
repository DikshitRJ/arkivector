/*
 * write/batcher.h: packs mutation operations into transaction batches.
 */
#ifndef AV_WRITE_BATCHER_H
#define AV_WRITE_BATCHER_H

#include <stddef.h>
#include <stdint.h>

#include "arkiv/abi.h"
#include "core/arena.h"
#include "log.h"

typedef struct {
    size_t max_tx_bytes;   /* default ~120,000 bytes */
    size_t max_ops_per_tx; /* default 100 */
} av_batcher_config;

typedef struct {
    const av_mutation_op *ops;
    size_t n_ops;
    uint8_t *calldata;
    size_t calldata_len;
} av_op_batch;

/* Pack a slice of operations into batches obeying size and op count limits */
av_status av_batcher_pack(const av_mutation_op *ops, size_t n_ops, const av_batcher_config *cfg,
                          av_arena *arena, av_op_batch **out_batches, size_t *out_n_batches,
                          av_err *e);

#endif /* AV_WRITE_BATCHER_H */
