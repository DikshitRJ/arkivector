/*
 * test_fees.c: EIP-1559 fee computation from canned eth_feeHistory responses (issue #14).
 */
#include <string.h>

#include "write/fees.h"
#include "yyjson.h"

#include "greatest.h"

#define GWEI 1000000000ULL

static av_fee_config cfg_default(void)
{
    av_fee_config c;
    memset(&c, 0, sizeof c);
    av_fee_config_defaults(&c);
    return c;
}

/* Parse JSON text with av_fee_history_parse; returns the status. */
static av_status parse(const char *json, av_fee_history *h)
{
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    if (d == NULL)
        return AV_ERR;
    av_err e;
    av_err_clear(&e);
    av_status st = av_fee_history_parse(yyjson_doc_get_root(d), h, &e);
    yyjson_doc_free(d);
    return st;
}

/* Shape of a real Rabdi answer to eth_feeHistory(4, "latest", [50]). */
static const char *k_history =
    "{\"oldestBlock\":\"0x8de1a0\","
    "\"baseFeePerGas\":[\"0x3b9aca00\",\"0x3b9aca00\",\"0x4a817c80\",\"0x3b9aca00\",\"0x77359400\"],"
    "\"gasUsedRatio\":[0.1,0.9,0.2,0.0],"
    "\"reward\":[[\"0x5f5e100\"],[\"0x2faf080\"],[\"0xbebc200\"],[\"0x5f5e100\"]]}";

TEST fees_from_history(void)
{
    av_fee_history h;
    ASSERT_EQ(AV_OK, parse(k_history, &h));
    ASSERT_EQ(4, h.n_blocks);
    ASSERT(h.has_reward);
    av_fee_config c = cfg_default();
    av_fee_quote q;
    ASSERT(av_fee_from_history(&h, &c, &q));
    ASSERT_EQ(AV_FEE_SRC_FEE_HISTORY, q.source);
    /* rewards 0.1, 0.05, 0.2, 0.1 gwei: median (upper) = 0.1 gwei */
    ASSERT_EQ(100000000ULL, q.max_priority_fee_per_gas);
    /* next base fee = last entry = 2 gwei: max fee = 2 * 2 + 0.1 */
    ASSERT_EQ(2 * GWEI, q.base_fee);
    ASSERT_EQ(4 * GWEI + 100000000ULL, q.max_fee_per_gas);
    PASS();
}

TEST fees_cap_and_floor(void)
{
    av_fee_history h;
    ASSERT_EQ(AV_OK, parse(k_history, &h));
    av_fee_config c = cfg_default();
    c.max_fee_cap = 3 * GWEI;
    av_fee_quote q;
    ASSERT(av_fee_from_history(&h, &c, &q));
    ASSERT_EQ(3 * GWEI, q.max_fee_per_gas);
    ASSERT(q.max_priority_fee_per_gas <= q.max_fee_per_gas);

    /* a quiet chain (zero rewards, zero base fee) still quotes a minimal tip */
    ASSERT_EQ(AV_OK, parse("{\"baseFeePerGas\":[\"0x0\",\"0x0\"],\"reward\":[[\"0x0\"]]}", &h));
    c.max_fee_cap = 0;
    ASSERT(av_fee_from_history(&h, &c, &q));
    ASSERT_EQ(AV_FEE_MIN_PRIORITY_WEI, q.max_priority_fee_per_gas);
    ASSERT_EQ(AV_FEE_MIN_PRIORITY_WEI, q.max_fee_per_gas);
    PASS();
}

TEST fees_history_without_rewards(void)
{
    av_fee_history h;
    ASSERT_EQ(AV_OK, parse("{\"baseFeePerGas\":[\"0x3b9aca00\",\"0x3b9aca00\"],\"gasUsedRatio\":[0.5]}", &h));
    ASSERT_FALSE(h.has_reward);
    av_fee_config c = cfg_default();
    av_fee_quote q;
    ASSERT(av_fee_from_history(&h, &c, &q));
    ASSERT_EQ(c.static_priority_fee, q.max_priority_fee_per_gas);
    ASSERT_EQ(2 * GWEI + c.static_priority_fee, q.max_fee_per_gas);
    PASS();
}

TEST fees_history_malformed(void)
{
    av_fee_history h;
    ASSERT_EQ(AV_ERR, parse("[]", &h));
    ASSERT_EQ(AV_ERR, parse("{}", &h));
    ASSERT_EQ(AV_ERR, parse("{\"baseFeePerGas\":[]}", &h));
    ASSERT_EQ(AV_ERR, parse("{\"baseFeePerGas\":[\"12\"]}", &h));
    ASSERT_EQ(AV_ERR, parse("{\"baseFeePerGas\":[\"0x1\"],\"reward\":[\"0x1\"]}", &h));
    ASSERT_EQ(AV_ERR, parse("{\"baseFeePerGas\":[\"0x1\"],\"reward\":[[]]}", &h));
    ASSERT_EQ(AV_ERR, parse("{\"baseFeePerGas\":[\"0x1ffffffffffffffff\"]}", &h));
    PASS();
}

TEST fees_gas_price_and_static(void)
{
    av_fee_config c = cfg_default();
    av_fee_quote q;
    av_fee_from_gas_price(1 * GWEI, &c, &q);
    ASSERT_EQ(AV_FEE_SRC_GAS_PRICE, q.source);
    ASSERT_EQ(2 * GWEI, q.max_fee_per_gas);
    ASSERT_EQ(1 * GWEI, q.max_priority_fee_per_gas); /* min(1.5 gwei static, 1 gwei) */
    av_fee_static(&c, &q);
    ASSERT_EQ(AV_FEE_SRC_STATIC, q.source);
    ASSERT_EQ(20 * GWEI, q.max_fee_per_gas);
    ASSERT_EQ(1500000000ULL, q.max_priority_fee_per_gas);
    PASS();
}

TEST fees_bump(void)
{
    av_fee_config c = cfg_default();
    av_fee_quote old = {.max_fee_per_gas = 1000, .max_priority_fee_per_gas = 9};
    av_fee_quote fresh = {.max_fee_per_gas = 900, .max_priority_fee_per_gas = 5};
    av_fee_quote out;
    av_fee_bump(&old, &fresh, &c, &out);
    ASSERT_EQ(1125, out.max_fee_per_gas);
    ASSERT_EQ(11, out.max_priority_fee_per_gas); /* ceil(9 * 1.125) = 11 */
    /* a higher fresh quote wins */
    fresh.max_fee_per_gas = 5000;
    fresh.max_priority_fee_per_gas = 50;
    av_fee_bump(&old, &fresh, &c, &out);
    ASSERT_EQ(5000, out.max_fee_per_gas);
    ASSERT_EQ(50, out.max_priority_fee_per_gas);
    /* the cap applies, but never below the minimum replacement bump */
    c.max_fee_cap = 2000;
    av_fee_bump(&old, &fresh, &c, &out);
    ASSERT_EQ(2000, out.max_fee_per_gas);
    c.max_fee_cap = 1100;
    av_fee_bump(&old, &fresh, &c, &out);
    ASSERT_EQ(1125, out.max_fee_per_gas);
    PASS();
}

SUITE(fees)
{
    RUN_TEST(fees_from_history);
    RUN_TEST(fees_cap_and_floor);
    RUN_TEST(fees_history_without_rewards);
    RUN_TEST(fees_history_malformed);
    RUN_TEST(fees_gas_price_and_static);
    RUN_TEST(fees_bump);
}
