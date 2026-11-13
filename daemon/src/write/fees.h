/*
 * write/fees.h: EIP-1559 fee quotes for managed writes (GitHub issue #14).
 *
 * Fee source chain, best first:
 *   1. eth_feeHistory over the last `history_blocks` blocks at `priority_percentile`:
 *      maxPriorityFeePerGas = median over the blocks of the percentile reward,
 *      maxFeePerGas         = 2 * next base fee + maxPriorityFeePerGas.
 *   2. eth_gasPrice: maxPriorityFeePerGas = min(static priority, gasPrice), maxFeePerGas = 2 * gasPrice.
 *   3. the static values from the configuration.
 * Every quote is clamped to `max_fee_cap` (maxFeePerGas) and keeps priority <= maxFee.
 * Replacements of stuck transactions bump both fees by at least 12.5% (geth's replacement rule
 * needs +10%; the extra keeps us clear of rounding in other clients).
 *
 * Ownership: plain values, nothing allocated. Thread-safe (no shared state).
 * Untrusted input: av_fee_history_parse reads a node response; fuzzed by fuzz_rpc_response.
 */
#ifndef AV_WRITE_FEES_H
#define AV_WRITE_FEES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/rpc.h"
#include "log.h"

struct yyjson_val;

#define AV_FEE_HISTORY_MAX_BLOCKS 1024
#define AV_FEE_MIN_PRIORITY_WEI   1000000ULL /* 0.001 gwei: never quote a zero tip */

typedef enum { AV_FEE_SRC_FEE_HISTORY = 0, AV_FEE_SRC_GAS_PRICE = 1, AV_FEE_SRC_STATIC = 2 } av_fee_source;

typedef struct {
    uint64_t max_fee_cap;         /* wei; 0 = no cap */
    double priority_percentile;   /* 0..100, default 50 */
    uint32_t history_blocks;      /* default 20 */
    uint64_t static_max_fee;      /* wei, last resort */
    uint64_t static_priority_fee; /* wei, last resort */
} av_fee_config;

typedef struct {
    uint64_t max_fee_per_gas;
    uint64_t max_priority_fee_per_gas;
    uint64_t base_fee; /* next block's base fee when known (fee history), else 0 */
    av_fee_source source;
} av_fee_quote;

/* Parsed eth_feeHistory result: base fees (n_blocks + 1 entries) and the first reward column. */
typedef struct {
    size_t n_blocks;
    uint64_t base_fee[AV_FEE_HISTORY_MAX_BLOCKS + 1];
    uint64_t reward[AV_FEE_HISTORY_MAX_BLOCKS];
    bool has_reward;
} av_fee_history;

const char *av_fee_source_name(av_fee_source s);

/* Fill unset fields with defaults (percentile 50, 20 blocks, 20 gwei / 1.5 gwei static fees). */
void av_fee_config_defaults(av_fee_config *cfg);

/* Decode an eth_feeHistory result object. AV_ERR ("arkiv_rpc_error") on any malformed field. */
av_status av_fee_history_parse(struct yyjson_val *res, av_fee_history *out, av_err *e);

/* Pure fee computation from a parsed history (source = fee history). False when the history is
 * unusable (no blocks, no base fee). */
bool av_fee_from_history(const av_fee_history *h, const av_fee_config *cfg, av_fee_quote *out);
/* Quote from eth_gasPrice (source = gas price). */
void av_fee_from_gas_price(uint64_t gas_price, const av_fee_config *cfg, av_fee_quote *out);
/* Quote from the static configuration (source = static). */
void av_fee_static(const av_fee_config *cfg, av_fee_quote *out);

/* Ask the node: fee history, then gas price, then static values. Never fails. */
void av_fee_quote_rpc(av_rpc *rpc, const av_fee_config *cfg, av_fee_quote *out);

/* Fees for a replacement of `old`: max(old * 1.125 (rounded up), fresh) for each fee, capped like
 * a quote (the cap never lowers a replacement below old * 1.125, or the node would reject it). */
void av_fee_bump(const av_fee_quote *old, const av_fee_quote *fresh, const av_fee_config *cfg,
                 av_fee_quote *out);

#endif /* AV_WRITE_FEES_H */
