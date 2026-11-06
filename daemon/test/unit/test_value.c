/*
 * test_value.c: canonical typed values (filter/value.c): parsing, printing, ordering.
 */
#include <stdio.h>
#include <string.h>

#include "filter/value.h"

#include "greatest.h"

static bool P(av_attr_type t, const char *s, av_value *v)
{
    return av_value_parse(t, s, strlen(s), v, NULL);
}

static const char *F(const av_value *v)
{
    static char buf[4][AV_VALUE_FORMAT_MAX];
    static int k;
    k = (k + 1) % 4;
    av_value_format(v, buf[k], sizeof(buf[k]));
    return buf[k];
}

/* parse then print; expects the canonical form `want` */
static int roundtrip(av_attr_type t, const char *in, const char *want)
{
    av_value v;
    if (!P(t, in, &v)) {
        return 0;
    }
    return strcmp(F(&v), want) == 0;
}

static void word_hex(const av_value *v, char out[65])
{
    for (int i = 0; i < 32; i++) {
        snprintf(out + 2 * i, 3, "%02x", v->b[i]);
    }
}

TEST dec_canonical_bytes(void)
{
    av_value v;
    char h[65];
    ASSERT(P(AV_AT_DEC, "4.5", &v));
    word_hex(&v, h);
    ASSERT_STR_EQ("0000000000000000000000000000000000000000000000003e73362871420000", h);
    ASSERT_EQ(32u, v.len);
    ASSERT(P(AV_AT_DEC, "-2.1", &v));
    word_hex(&v, h);
    ASSERT_STR_EQ("ffffffffffffffffffffffffffffffffffffffffffffffffe2db4d2053ae0000", h);
    ASSERT(P(AV_AT_DEC, "1", &v));
    word_hex(&v, h);
    ASSERT_STR_EQ("0000000000000000000000000000000000000000000000000de0b6b3a7640000", h);
    PASS();
}

TEST dec_print_trimmed(void)
{
    ASSERT(roundtrip(AV_AT_DEC, "4.0", "4"));
    ASSERT(roundtrip(AV_AT_DEC, "4.50", "4.5"));
    ASSERT(roundtrip(AV_AT_DEC, "00012.50", "12.5"));
    ASSERT(roundtrip(AV_AT_DEC, "0.000000000000000001", "0.000000000000000001"));
    ASSERT(roundtrip(AV_AT_DEC, "-0.000000000000000001", "-0.000000000000000001"));
    ASSERT(roundtrip(AV_AT_DEC, "123456789.123456789012345678", "123456789.123456789012345678"));
    ASSERT(roundtrip(AV_AT_DEC, "-0.25", "-0.25"));
    ASSERT(roundtrip(AV_AT_DEC, "0", "0"));
    PASS();
}

TEST dec_negative_zero(void)
{
    av_value z, nz, nz2;
    ASSERT(P(AV_AT_DEC, "0", &z));
    ASSERT(P(AV_AT_DEC, "-0", &nz));
    ASSERT(P(AV_AT_DEC, "-0.000", &nz2));
    ASSERT_EQ(0, memcmp(z.b, nz.b, 32));
    ASSERT_EQ(0, memcmp(z.b, nz2.b, 32));
    ASSERT_STR_EQ("0", F(&nz));
    ASSERT(av_value_equal(&z, &nz));
    PASS();
}

TEST dec_limits(void)
{
    av_value v;
    const char *mx = "57896044618658097711785492504343953926634992332820282019728.792003956564819967";
    const char *mn = "-57896044618658097711785492504343953926634992332820282019728.792003956564819968";
    ASSERT(roundtrip(AV_AT_DEC, mx, mx));
    ASSERT(roundtrip(AV_AT_DEC, mn, mn));
    ASSERT(!P(AV_AT_DEC, "57896044618658097711785492504343953926634992332820282019728.792003956564819968", &v));
    ASSERT(!P(AV_AT_DEC, "-57896044618658097711785492504343953926634992332820282019728.792003956564819969", &v));
    ASSERT(!P(AV_AT_DEC, "99999999999999999999999999999999999999999999999999999999999999999999999999999999", &v));
    /* 18 fraction digits ok, 19 rejected (even when the 19th is 0) */
    ASSERT(P(AV_AT_DEC, "1.123456789012345678", &v));
    ASSERT(!P(AV_AT_DEC, "1.1234567890123456780", &v));
    ASSERT(!P(AV_AT_DEC, "0.0000000000000000000", &v));
    /* leading zeros never overflow */
    ASSERT(P(AV_AT_DEC, "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000001", &v));
    PASS();
}

TEST dec_rejects_malformed(void)
{
    static const char *bad[] = {"", "-", ".5", "5.", "+1", " 1", "1 ", "1e3", "1.2.3", "0x10", "--1", "1,5", "abc", "-.5", "1_0"};
    av_value v;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERTm(bad[i], !P(AV_AT_DEC, bad[i], &v));
    }
    PASS();
}

TEST dec_ordering(void)
{
    static const char *asc[] = {"-57896044618658097711785492504343953926634992332820282019728.792003956564819968",
                                "-1000", "-1", "-0.5", "-0.000000000000000001", "0", "0.000000000000000001", "0.5",
                                "1", "1000", "57896044618658097711785492504343953926634992332820282019728.792003956564819967"};
    for (size_t i = 0; i + 1 < sizeof(asc) / sizeof(asc[0]); i++) {
        av_value a, b;
        ASSERT(P(AV_AT_DEC, asc[i], &a));
        ASSERT(P(AV_AT_DEC, asc[i + 1], &b));
        ASSERTm(asc[i], av_value_cmp(&a, &b) < 0);
        ASSERTm(asc[i], av_value_cmp(&b, &a) > 0);
        ASSERT_EQ(0, av_value_cmp(&a, &a));
    }
    PASS();
}

TEST u256_parse_print(void)
{
    const char *mx = "115792089237316195423570985008687907853269984665640564039457584007913129639935";
    av_value v;
    ASSERT(roundtrip(AV_AT_U256, mx, mx));
    ASSERT(!P(AV_AT_U256, "115792089237316195423570985008687907853269984665640564039457584007913129639936", &v));
    ASSERT(roundtrip(AV_AT_U256, "0", "0"));
    ASSERT(roundtrip(AV_AT_U256, "1000000000000000000", "1000000000000000000"));
    ASSERT(roundtrip(AV_AT_U256, "0xff", "255"));
    ASSERT(roundtrip(AV_AT_U256, "0XFF", "255"));
    ASSERT(roundtrip(AV_AT_U256, "0x0", "0"));
    ASSERT(roundtrip(AV_AT_U256, "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", mx));
    ASSERT(!P(AV_AT_U256, "0x", &v));
    ASSERT(!P(AV_AT_U256, "0x1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", &v)); /* 65 digits */
    ASSERT(!P(AV_AT_U256, "0xzz", &v));
    ASSERT(!P(AV_AT_U256, "-1", &v));
    ASSERT(!P(AV_AT_U256, "", &v));
    ASSERT(!P(AV_AT_U256, "1.0", &v));
    char h[65];
    ASSERT(P(AV_AT_U256, "0x1", &v));
    word_hex(&v, h);
    ASSERT_STR_EQ("0000000000000000000000000000000000000000000000000000000000000001", h);
    ASSERT(P(AV_AT_U256, "0xabc", &v));
    ASSERT_EQ(0x0a, v.b[30]);
    ASSERT_EQ(0xbc, v.b[31]);
    PASS();
}

TEST i32_u64_ranges(void)
{
    av_value v, w;
    ASSERT(roundtrip(AV_AT_I32, "2147483647", "2147483647"));
    ASSERT(roundtrip(AV_AT_I32, "-2147483648", "-2147483648"));
    ASSERT(!P(AV_AT_I32, "2147483648", &v));
    ASSERT(!P(AV_AT_I32, "-2147483649", &v));
    ASSERT(!P(AV_AT_I32, "99999999999999999999999999", &v));
    ASSERT(!P(AV_AT_I32, "1.5", &v));
    ASSERT(!P(AV_AT_I32, "", &v));
    ASSERT(!P(AV_AT_I32, "-", &v));
    ASSERT(roundtrip(AV_AT_I32, "-0", "0"));
    ASSERT(roundtrip(AV_AT_U64, "18446744073709551615", "18446744073709551615"));
    ASSERT(!P(AV_AT_U64, "18446744073709551616", &v));
    ASSERT(!P(AV_AT_U64, "-1", &v));
    ASSERT(roundtrip(AV_AT_U64, "0", "0"));
    /* sign extension of i32 */
    ASSERT(P(AV_AT_I32, "-1", &v));
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(0xff, v.b[i]);
    }
    av_value_i32(&w, -1);
    ASSERT(av_value_equal(&v, &w));
    PASS();
}

TEST bool_values(void)
{
    av_value t, f;
    ASSERT(P(AV_AT_BOOL, "true", &t));
    ASSERT(P(AV_AT_BOOL, "FALSE", &f));
    ASSERT(av_value_cmp(&f, &t) < 0);
    ASSERT_STR_EQ("true", F(&t));
    ASSERT_STR_EQ("false", F(&f));
    ASSERT(!P(AV_AT_BOOL, "1", &t));
    ASSERT(!P(AV_AT_BOOL, "", &t));
    PASS();
}

TEST addr_values(void)
{
    av_value v, w;
    char h[65];
    ASSERT(P(AV_AT_ADDR, "0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045", &v)); /* valid EIP-55 */
    word_hex(&v, h);
    ASSERT_STR_EQ("000000000000000000000000d8da6bf26964af9d7eed9e03e53415d37aa96045", h);
    ASSERT_STR_EQ("0xd8da6bf26964af9d7eed9e03e53415d37aa96045", F(&v));
    ASSERT(P(AV_AT_ADDR, "0xd8da6bf26964af9d7eed9e03e53415d37aa96045", &w)); /* lower */
    ASSERT(av_value_equal(&v, &w));
    ASSERT(P(AV_AT_ADDR, "0xD8DA6BF26964AF9D7EED9E03E53415D37AA96045", &w)); /* upper */
    ASSERT(av_value_equal(&v, &w));
    ASSERT(P(AV_AT_ADDR, "0XD8DA6BF26964AF9D7EED9E03E53415D37AA96045", &w));
    ASSERT(!P(AV_AT_ADDR, "0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96046", &w)); /* wrong body */
    ASSERT(!P(AV_AT_ADDR, "0xD8dA6BF26964aF9D7eEd9e03E53415D37aA96045", &w)); /* flipped case: bad checksum */
    ASSERT(!P(AV_AT_ADDR, "0xd8da6bf26964af9d7eed9e03e53415d37aa9604", &w));  /* 39 digits */
    ASSERT(!P(AV_AT_ADDR, "0xd8da6bf26964af9d7eed9e03e53415d37aa960455", &w)); /* 41 digits */
    ASSERT(!P(AV_AT_ADDR, "d8da6bf26964af9d7eed9e03e53415d37aa96045", &w));
    ASSERT(!P(AV_AT_ADDR, "0xg8da6bf26964af9d7eed9e03e53415d37aa96045", &w));
    ASSERT(P(AV_AT_ADDR, "0x1111111111111111111111111111111111111111", &w));
    /* another well-known EIP-55 vector (from the EIP) */
    ASSERT(P(AV_AT_ADDR, "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", &w));
    ASSERT(P(AV_AT_ADDR, "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359", &w));
    ASSERT(P(AV_AT_ADDR, "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB", &w));
    ASSERT(P(AV_AT_ADDR, "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb", &w));
    ASSERT(!P(AV_AT_ADDR, "0x5AAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", &w));
    PASS();
}

TEST key_values(void)
{
    av_value v, w;
    const char *k = "0x00000000000000000000000000000000000000000000000000000000000000ff";
    ASSERT(P(AV_AT_KEY, k, &v));
    ASSERT_STR_EQ(k, F(&v));
    ASSERT_EQ(0xff, v.b[31]);
    ASSERT(P(AV_AT_KEY, "0xFF00000000000000000000000000000000000000000000000000000000000000", &w));
    ASSERT(av_value_cmp(&v, &w) < 0); /* unsigned big-endian */
    ASSERT(!P(AV_AT_KEY, "0xff", &v));
    ASSERT(!P(AV_AT_KEY, "0x", &v));
    ASSERT(!P(AV_AT_KEY, "00000000000000000000000000000000000000000000000000000000000000ff", &v));
    PASS();
}

TEST str_values(void)
{
    av_value v;
    char big[200];
    memset(big, 'a', sizeof(big));
    ASSERT(av_value_parse(AV_AT_STR, big, 128, &v, NULL));
    ASSERT_EQ(128u, v.len);
    ASSERT(!av_value_parse(AV_AT_STR, big, 129, &v, NULL));
    ASSERT(av_value_parse(AV_AT_STR, "", 0, &v, NULL));
    ASSERT_EQ(0u, v.len);
    ASSERT(P(AV_AT_STR, "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80", &v));
    ASSERT(!av_value_parse(AV_AT_STR, "\xC0\x80", 2, &v, NULL));         /* overlong NUL */
    ASSERT(!av_value_parse(AV_AT_STR, "\xED\xA0\x80", 3, &v, NULL));     /* surrogate */
    ASSERT(!av_value_parse(AV_AT_STR, "\xF4\x90\x80\x80", 4, &v, NULL)); /* > U+10FFFF */
    ASSERT(!av_value_parse(AV_AT_STR, "\xE2\x82", 2, &v, NULL));         /* truncated */
    ASSERT(!av_value_parse(AV_AT_STR, "\x80", 1, &v, NULL));
    /* embedded NUL is valid UTF-8 and kept */
    ASSERT(av_value_parse(AV_AT_STR, "a\0b", 3, &v, NULL));
    ASSERT_EQ(3u, v.len);
    PASS();
}

TEST str_ordering_is_bytewise(void)
{
    av_value a, b;
    ASSERT(P(AV_AT_STR, "a", &a));
    ASSERT(P(AV_AT_STR, "ab", &b));
    ASSERT(av_value_cmp(&a, &b) < 0);
    ASSERT(P(AV_AT_STR, "Z", &a));
    ASSERT(P(AV_AT_STR, "a", &b));
    ASSERT(av_value_cmp(&a, &b) < 0); /* no collation: uppercase first */
    ASSERT(P(AV_AT_STR, "z", &a));
    ASSERT(P(AV_AT_STR, "\xC3\xA9", &b)); /* e acute */
    ASSERT(av_value_cmp(&a, &b) < 0);
    ASSERT(P(AV_AT_STR, "", &a));
    ASSERT(av_value_cmp(&a, &b) < 0);
    ASSERT(av_value_cmp(&a, &a) == 0);
    PASS();
}

TEST int_ordering(void)
{
    av_value a, b;
    ASSERT(P(AV_AT_I32, "-5", &a));
    ASSERT(P(AV_AT_I32, "3", &b));
    ASSERT(av_value_cmp(&a, &b) < 0);
    ASSERT(P(AV_AT_I32, "-2147483648", &a));
    ASSERT(P(AV_AT_I32, "-1", &b));
    ASSERT(av_value_cmp(&a, &b) < 0);
    ASSERT(P(AV_AT_U64, "18446744073709551615", &a));
    ASSERT(P(AV_AT_U64, "1", &b));
    ASSERT(av_value_cmp(&a, &b) > 0);
    ASSERT(P(AV_AT_U256, "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", &a));
    ASSERT(P(AV_AT_U256, "0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", &b));
    ASSERT(av_value_cmp(&a, &b) > 0); /* unsigned, not signed */
    PASS();
}

TEST cross_type_compare_orders_by_type(void)
{
    av_value a, b;
    ASSERT(P(AV_AT_I32, "1", &a));
    ASSERT(P(AV_AT_U64, "1", &b));
    ASSERT(av_value_cmp(&a, &b) != 0);
    ASSERT(!av_value_equal(&a, &b));
    PASS();
}

TEST builders_match_parse(void)
{
    av_value a, b;
    uint8_t addr[20];
    memset(addr, 0x11, sizeof(addr));
    av_value_addr(&a, addr);
    ASSERT(P(AV_AT_ADDR, "0x1111111111111111111111111111111111111111", &b));
    ASSERT(av_value_equal(&a, &b));
    av_value_u64(&a, 1750000);
    ASSERT(P(AV_AT_U64, "1750000", &b));
    ASSERT(av_value_equal(&a, &b));
    av_value_bool(&a, true);
    ASSERT(P(AV_AT_BOOL, "true", &b));
    ASSERT(av_value_equal(&a, &b));
    av_value_i32(&a, INT32_MIN);
    ASSERT(P(AV_AT_I32, "-2147483648", &b));
    ASSERT(av_value_equal(&a, &b));
    PASS();
}

TEST from_stored_checks_canonical_form(void)
{
    av_value v;
    uint8_t w[32];
    memset(w, 0, sizeof(w));
    w[31] = 1;
    ASSERT(av_value_from_stored(AV_AT_BOOL, w, 32, &v));
    w[31] = 2;
    ASSERT(!av_value_from_stored(AV_AT_BOOL, w, 32, &v));
    ASSERT(!av_value_from_stored(AV_AT_BOOL, w, 31, &v));
    memset(w, 0xff, sizeof(w)); /* i32 -1 */
    ASSERT(av_value_from_stored(AV_AT_I32, w, 32, &v));
    ASSERT_STR_EQ("-1", F(&v));
    w[0] = 0x00; /* not sign-extended */
    ASSERT(!av_value_from_stored(AV_AT_I32, w, 32, &v));
    memset(w, 0, sizeof(w));
    w[3] = 1;
    ASSERT(!av_value_from_stored(AV_AT_U64, w, 32, &v));
    ASSERT(!av_value_from_stored(AV_AT_ADDR, w, 32, &v));
    ASSERT(av_value_from_stored(AV_AT_KEY, w, 32, &v));
    ASSERT(av_value_from_stored(AV_AT_DEC, w, 32, &v));
    ASSERT(av_value_from_stored(AV_AT_STR, (const uint8_t *) "abc", 3, &v));
    ASSERT(!av_value_from_stored(AV_AT_STR, (const uint8_t *) "\xff", 1, &v));
    ASSERT(!av_value_from_stored(AV_AT_BYTES, w, 32, &v));
    PASS();
}

TEST format_truncation_follows_snprintf(void)
{
    av_value v;
    char buf[8];
    ASSERT(P(AV_AT_U256, "123456789012", &v));
    ASSERT_EQ(12u, av_value_format(&v, buf, sizeof(buf)));
    ASSERT_STR_EQ("1234567", buf);
    ASSERT_EQ(12u, av_value_format(&v, NULL, 0));
    PASS();
}

TEST type_names(void)
{
    av_attr_type t;
    ASSERT(av_type_from_name("dec", 3, &t));
    ASSERT_EQ(AV_AT_DEC, t);
    ASSERT(av_type_from_name("bytes", 5, &t));
    ASSERT(!av_type_from_name("DEC", 3, &t));
    ASSERT(!av_type_from_name("decx", 4, &t));
    ASSERT(!av_type_from_name("", 0, &t));
    ASSERT_STR_EQ("u256", av_type_name(AV_AT_U256));
    ASSERT(!av_type_filterable(AV_AT_BYTES));
    ASSERT(av_type_filterable(AV_AT_KEY));
    PASS();
}

SUITE(value)
{
    RUN_TEST(dec_canonical_bytes);
    RUN_TEST(dec_print_trimmed);
    RUN_TEST(dec_negative_zero);
    RUN_TEST(dec_limits);
    RUN_TEST(dec_rejects_malformed);
    RUN_TEST(dec_ordering);
    RUN_TEST(u256_parse_print);
    RUN_TEST(i32_u64_ranges);
    RUN_TEST(bool_values);
    RUN_TEST(addr_values);
    RUN_TEST(key_values);
    RUN_TEST(str_values);
    RUN_TEST(str_ordering_is_bytewise);
    RUN_TEST(int_ordering);
    RUN_TEST(cross_type_compare_orders_by_type);
    RUN_TEST(builders_match_parse);
    RUN_TEST(from_stored_checks_canonical_form);
    RUN_TEST(format_truncation_follows_snprintf);
    RUN_TEST(type_names);
}
