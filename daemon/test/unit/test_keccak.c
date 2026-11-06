/*
 * test_keccak.c: Keccak-256 (Ethereum padding) known answers, rate-boundary lengths and the five
 * Arkiv event topic0 constants from RESEARCH/arkiv-docs/03-evm-mutations-and-contract-spec/events-and-logs.md.
 */
#include <stdio.h>
#include <string.h>

#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "arkiv/logs.h"
#include "greatest.h"

static void hex_of(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
}

static int check_vec(const char *in, size_t len, const char *want_hex)
{
    uint8_t d[32];
    char h[65];
    av_keccak256(in, len, d);
    hex_of(d, 32, h);
    return strcmp(h, want_hex) == 0;
}

TEST known_answers(void)
{
    ASSERT(check_vec("", 0, "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"));
    ASSERT(check_vec("abc", 3, "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45"));
    ASSERT(check_vec("transfer(address,uint256)", strlen("transfer(address,uint256)"),
                     "a9059cbb2ab09eb219583f4a59a5d0623ade346d962bcd4e46b11da047c9049b"));
    PASS();
}

TEST rate_boundaries(void)
{
    /* Lengths around the 136-byte rate must agree with a byte-at-a-time reference run:
       hash(prefix of a 300-byte buffer) must differ for every length and be deterministic. */
    uint8_t buf[300];
    uint8_t prev[32] = {0};
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t) (i * 7 + 1);
    for (size_t n = 130; n <= 280; n++) {
        uint8_t a[32];
        uint8_t b[32];
        av_keccak256(buf, n, a);
        av_keccak256(buf, n, b);
        ASSERT(memcmp(a, b, 32) == 0);
        ASSERT(memcmp(a, prev, 32) != 0);
        memcpy(prev, a, 32);
    }
    PASS();
}

TEST event_topics(void)
{
    static const char *sigs[] = {"EntityCreated(bytes32,address,uint64,uint8)",
                                 "EntityPatched(bytes32,address)", "EntityDeleted(bytes32,address)",
                                 "ExpiryExtended(bytes32,address,uint64)",
                                 "OwnershipTransferred(bytes32,address,address)"};
    static const char *want[] = {"b282d7c494b8899aa8015cd07be621530beb03409eb8c5e8fdc1411ba64356a5",
                                 "e986ee220b2d7947a009d9389d8400b2ac82d3b430a9f72700a0975d3d5a34fe",
                                 "4059b76c47e1ecc40ba88e649b654f716515eb358c3ce83c803820e3b3130cc3",
                                 "10dc3526654c9ba4e2370b1bfcbc0de087d141a5694b3d3305d37df7e6705d4b",
                                 "0b659dccc8eb950324170e8d9598af5ee04ee070883eb28651a96788721fbf83"};
    for (int i = 0; i < 5; i++) {
        ASSERT(check_vec(sigs[i], strlen(sigs[i]), want[i]));
        const uint8_t *t = av_event_topic0((av_event_kind) (i + 1));
        char h[65];
        ASSERT(t != NULL);
        hex_of(t, 32, h);
        ASSERT_STR_EQ(want[i], h);
    }
    ASSERT(av_event_topic0((av_event_kind) 0) == NULL);
    ASSERT(av_event_topic0((av_event_kind) 6) == NULL);
    PASS();
}

SUITE(keccak)
{
    RUN_TEST(known_answers);
    RUN_TEST(rate_boundaries);
    RUN_TEST(event_topics);
}
