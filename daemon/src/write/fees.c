/*
 * write/fees.c: EIP-1559 fee quotes. See fees.h.
 */
#include "write/fees.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "yyjson.h"

#define GWEI 1000000000ULL

const char *av_fee_source_name(av_fee_source s)
{
    switch (s) {
    case AV_FEE_SRC_FEE_HISTORY:
        return "fee_history";
    case AV_FEE_SRC_GAS_PRICE:
        return "gas_price";
    case AV_FEE_SRC_STATIC:
        return "static";
    }
    return "static";
}

void av_fee_config_defaults(av_fee_config *cfg)
{
    if (cfg->priority_percentile <= 0.0 || cfg->priority_percentile > 100.0)
        cfg->priority_percentile = 50.0;
    if (cfg->history_blocks == 0 || cfg->history_blocks > AV_FEE_HISTORY_MAX_BLOCKS)
        cfg->history_blocks = 20;
    if (cfg->static_max_fee == 0)
        cfg->static_max_fee = 20 * GWEI;
    if (cfg->static_priority_fee == 0)
        cfg->static_priority_fee = 1500000000ULL; /* 1.5 gwei */
}

static bool qty_val(yyjson_val *v, uint64_t *out)
{
    const char *s = yyjson_get_str(v);
    return s != NULL && av_qty_parse(s, out);
}

av_status av_fee_history_parse(yyjson_val *res, av_fee_history *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (!yyjson_is_obj(res)) {
        av_err_set(e, "arkiv_rpc_error", "eth_feeHistory: result is not an object");
        return AV_ERR;
    }
    yyjson_val *bf = yyjson_obj_get(res, "baseFeePerGas");
    if (!yyjson_is_arr(bf) || yyjson_arr_size(bf) < 1 ||
        yyjson_arr_size(bf) > AV_FEE_HISTORY_MAX_BLOCKS + 1) {
        av_err_set(e, "arkiv_rpc_error", "eth_feeHistory: baseFeePerGas missing or too long");
        return AV_ERR;
    }
    size_t nb = yyjson_arr_size(bf);
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(bf, idx, max, v)
    {
        if (!qty_val(v, &out->base_fee[idx])) {
            av_err_set(e, "arkiv_rpc_error", "eth_feeHistory: baseFeePerGas[%zu] is not a quantity", idx);
            return AV_ERR;
        }
    }
    out->n_blocks = nb - 1;
    yyjson_val *rw = yyjson_obj_get(res, "reward");
    if (rw != NULL && !yyjson_is_null(rw)) {
        if (!yyjson_is_arr(rw) || yyjson_arr_size(rw) > AV_FEE_HISTORY_MAX_BLOCKS) {
            av_err_set(e, "arkiv_rpc_error", "eth_feeHistory: reward is not an array");
            return AV_ERR;
        }
        size_t nr = yyjson_arr_size(rw);
        yyjson_arr_foreach(rw, idx, max, v)
        {
            if (!yyjson_is_arr(v) || yyjson_arr_size(v) < 1 ||
                !qty_val(yyjson_arr_get_first(v), &out->reward[idx])) {
                av_err_set(e, "arkiv_rpc_error", "eth_feeHistory: reward[%zu] is malformed", idx);
                return AV_ERR;
            }
        }
        if (nr > 0) {
            out->has_reward = true;
            /* Nodes return one reward row per block; trust the shorter of the two lengths. */
            if (nr < out->n_blocks)
                out->n_blocks = nr;
        }
    }
    return AV_OK;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static uint64_t sat_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static uint64_t sat_mul2(uint64_t a)
{
    return a > UINT64_MAX / 2 ? UINT64_MAX : a * 2;
}

static void clamp_quote(av_fee_quote *q, const av_fee_config *cfg)
{
    if (q->max_priority_fee_per_gas < AV_FEE_MIN_PRIORITY_WEI)
        q->max_priority_fee_per_gas = AV_FEE_MIN_PRIORITY_WEI;
    if (q->max_fee_per_gas < q->max_priority_fee_per_gas)
        q->max_fee_per_gas = q->max_priority_fee_per_gas;
    if (cfg->max_fee_cap > 0 && q->max_fee_per_gas > cfg->max_fee_cap)
        q->max_fee_per_gas = cfg->max_fee_cap;
    if (q->max_priority_fee_per_gas > q->max_fee_per_gas)
        q->max_priority_fee_per_gas = q->max_fee_per_gas;
}

bool av_fee_from_history(const av_fee_history *h, const av_fee_config *cfg, av_fee_quote *out)
{
    memset(out, 0, sizeof(*out));
    if (h == NULL)
        return false;
    uint64_t next_base = h->base_fee[h->n_blocks]; /* last entry: the block after newest */
    uint64_t prio = cfg->static_priority_fee;
    if (h->has_reward && h->n_blocks > 0) {
        uint64_t sorted[AV_FEE_HISTORY_MAX_BLOCKS];
        memcpy(sorted, h->reward, h->n_blocks * sizeof(uint64_t));
        qsort(sorted, h->n_blocks, sizeof(uint64_t), cmp_u64);
        prio = sorted[h->n_blocks / 2];
    }
    out->base_fee = next_base;
    out->max_priority_fee_per_gas = prio;
    out->max_fee_per_gas = sat_add(sat_mul2(next_base), prio);
    out->source = AV_FEE_SRC_FEE_HISTORY;
    clamp_quote(out, cfg);
    return true;
}

void av_fee_from_gas_price(uint64_t gas_price, const av_fee_config *cfg, av_fee_quote *out)
{
    memset(out, 0, sizeof(*out));
    out->max_priority_fee_per_gas =
        gas_price < cfg->static_priority_fee ? gas_price : cfg->static_priority_fee;
    out->max_fee_per_gas = sat_mul2(gas_price);
    out->source = AV_FEE_SRC_GAS_PRICE;
    clamp_quote(out, cfg);
}

void av_fee_static(const av_fee_config *cfg, av_fee_quote *out)
{
    memset(out, 0, sizeof(*out));
    out->max_fee_per_gas = cfg->static_max_fee;
    out->max_priority_fee_per_gas = cfg->static_priority_fee;
    out->source = AV_FEE_SRC_STATIC;
    clamp_quote(out, cfg);
}

void av_fee_quote_rpc(av_rpc *rpc, const av_fee_config *cfg, av_fee_quote *out)
{
    if (rpc != NULL) {
        char params[96];
        snprintf(params, sizeof(params), "[\"0x%x\",\"latest\",[%.2f]]", (unsigned) cfg->history_blocks,
                 cfg->priority_percentile);
        yyjson_doc *doc = NULL;
        yyjson_val *res = NULL;
        int code = 0;
        av_err ee;
        av_err_clear(&ee);
        if (av_rpc_call(rpc, "eth_feeHistory", params, &doc, &res, &code, &ee) == AV_OK) {
            av_fee_history *h = malloc(sizeof(*h));
            bool ok =
                h != NULL && av_fee_history_parse(res, h, &ee) == AV_OK && av_fee_from_history(h, cfg, out);
            free(h);
            yyjson_doc_free(doc);
            if (ok)
                return;
            av_log(AV_LOG_WARN, "writer", "eth_feeHistory unusable (%s); trying eth_gasPrice", ee.msg);
        } else {
            av_log(AV_LOG_DEBUG, "writer", "eth_feeHistory failed (%s); trying eth_gasPrice", ee.msg);
        }
        doc = NULL;
        res = NULL;
        av_err_clear(&ee);
        if (av_rpc_call(rpc, "eth_gasPrice", "[]", &doc, &res, &code, &ee) == AV_OK) {
            uint64_t gp = 0;
            bool ok = qty_val(res, &gp) && gp > 0;
            yyjson_doc_free(doc);
            if (ok) {
                av_fee_from_gas_price(gp, cfg, out);
                return;
            }
        }
        av_log(AV_LOG_WARN, "writer", "eth_gasPrice failed (%s); using the static fees", ee.msg);
    }
    av_fee_static(cfg, out);
}

static uint64_t bump_125(uint64_t v)
{
    /* ceil(v * 1.125) without overflow */
    uint64_t add = v / 8 + (v % 8 != 0);
    return sat_add(v, add);
}

void av_fee_bump(const av_fee_quote *old, const av_fee_quote *fresh, const av_fee_config *cfg,
                 av_fee_quote *out)
{
    uint64_t min_fee = bump_125(old->max_fee_per_gas);
    uint64_t min_prio = bump_125(old->max_priority_fee_per_gas);
    *out = *fresh;
    if (out->max_fee_per_gas < min_fee)
        out->max_fee_per_gas = min_fee;
    if (out->max_priority_fee_per_gas < min_prio)
        out->max_priority_fee_per_gas = min_prio;
    if (cfg->max_fee_cap > 0 && out->max_fee_per_gas > cfg->max_fee_cap)
        out->max_fee_per_gas = cfg->max_fee_cap > min_fee ? cfg->max_fee_cap : min_fee;
    if (out->max_priority_fee_per_gas > out->max_fee_per_gas)
        out->max_priority_fee_per_gas = out->max_fee_per_gas;
}
