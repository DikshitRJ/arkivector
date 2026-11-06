/*
 * test_rlp.c: RLP vectors from the Ethereum wiki / ethereum/tests rlptest.json.
 */
#include <string.h>

#include "arkiv/rlp.h"
#include "codec/hex.h"

#include "greatest.h"

static int matches(av_rlp *r, const char *hex)
{
    const uint8_t *out;
    size_t len;
    av_err e;
    if (av_rlp_finish(r, &out, &len, &e) != AV_OK) {
        return 0;
    }
    char buf[4096];
    size_t n = 0;
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len && n + 2 < sizeof(buf); i++) {
        buf[n++] = digits[out[i] >> 4];
        buf[n++] = digits[out[i] & 15];
    }
    buf[n] = '\0';
    return strcmp(buf, hex) == 0;
}

TEST rlp_scalars(void)
{
    av_rlp r;
    av_rlp_init(&r);
    av_rlp_bytes(&r, (const uint8_t *) "dog", 3);
    ASSERT(matches(&r, "83646f67"));
    av_rlp_free(&r);

    av_rlp_init(&r);
    av_rlp_bytes(&r, NULL, 0); /* empty string */
    ASSERT(matches(&r, "80"));
    av_rlp_free(&r);

    av_rlp_init(&r);
    av_rlp_u64(&r, 0);
    ASSERT(matches(&r, "80"));
    av_rlp_free(&r);

    av_rlp_init(&r);
    av_rlp_u64(&r, 15);
    ASSERT(matches(&r, "0f"));
    av_rlp_free(&r);

    av_rlp_init(&r);
    av_rlp_u64(&r, 1024);
    ASSERT(matches(&r, "820400"));
    av_rlp_free(&r);

    av_rlp_init(&r);
    av_rlp_u64(&r, 0xffffffffffffffffULL);
    ASSERT(matches(&r, "88ffffffffffffffff"));
    av_rlp_free(&r);

    /* 56-byte string uses the long form */
    const char *lorem = "Lorem ipsum dolor sit amet, consectetur adipisicing elit";
    av_rlp_init(&r);
    av_rlp_bytes(&r, (const uint8_t *) lorem, strlen(lorem));
    ASSERT(matches(&r, "b8384c6f72656d20697073756d20646f6c6f722073697420616d65742c20636f6e7365637465747572206"
                       "164697069736963696e6720656c6974"));
    av_rlp_free(&r);

    uint8_t big[32] = {0};
    big[31] = 1;
    big[30] = 0;
    av_rlp_init(&r);
    av_rlp_u256(&r, big);
    ASSERT(matches(&r, "01"));
    av_rlp_free(&r);
    PASS();
}

TEST rlp_lists(void)
{
    av_rlp r;
    av_rlp_init(&r);
    av_rlp_list_begin(&r);
    av_rlp_bytes(&r, (const uint8_t *) "cat", 3);
    av_rlp_bytes(&r, (const uint8_t *) "dog", 3);
    av_rlp_list_end(&r);
    ASSERT(matches(&r, "c88363617483646f67"));
    av_rlp_free(&r);

    av_rlp_init(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_end(&r);
    ASSERT(matches(&r, "c0"));
    av_rlp_free(&r);

    /* set theoretic representation of three: [ [], [[]], [ [], [[]] ] ] */
    av_rlp_init(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_end(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_end(&r);
    av_rlp_list_end(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_end(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_begin(&r);
    av_rlp_list_end(&r);
    av_rlp_list_end(&r);
    av_rlp_list_end(&r);
    av_rlp_list_end(&r);
    ASSERT(matches(&r, "c7c0c1c0c3c0c1c0"));
    av_rlp_free(&r);

    /* long list (> 55 bytes payload) */
    av_rlp_init(&r);
    av_rlp_list_begin(&r);
    for (int i = 0; i < 4; i++) {
        av_rlp_list_begin(&r);
        av_rlp_bytes(&r, (const uint8_t *) "asdf", 4);
        av_rlp_bytes(&r, (const uint8_t *) "qwer", 4);
        av_rlp_bytes(&r, (const uint8_t *) "zxcv", 4);
        av_rlp_list_end(&r);
    }
    av_rlp_list_end(&r);
    ASSERT(matches(&r, "f840cf84617364668471776572847a786376cf84617364668471776572847a786376cf846173646684717"
                       "76572847a786376cf84617364668471776572847a786376"));
    av_rlp_free(&r);

    /* unclosed list is an error */
    av_rlp_init(&r);
    av_rlp_list_begin(&r);
    const uint8_t *o;
    size_t l;
    av_err e;
    ASSERT_EQ(AV_ERR, av_rlp_finish(&r, &o, &l, &e));
    av_rlp_free(&r);
    PASS();
}

SUITE(rlp)
{
    RUN_TEST(rlp_scalars);
    RUN_TEST(rlp_lists);
}
