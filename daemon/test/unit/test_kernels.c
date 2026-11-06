/*
 * test_kernels.c (D0 part): pgvector half-float conversion and bit distance kernels.
 */
#include "postgres.h"

#include "bitutils.h"
#include "halfutils.h"
#include "halfvec.h"
#include "port/pg_bitutils.h"

#include "greatest.h"

TEST half_roundtrip_all_patterns(void)
{
    for (uint32 bits = 0; bits <= 0xFFFF; bits++) {
        half h;
        uint16 b16 = (uint16) bits;
        memcpy(&h, &b16, sizeof(h));
        if (HalfIsNan(h))
            continue;
        float f = HalfToFloat4(h);
        half back = Float4ToHalfUnchecked(f);
        uint16 out;
        memcpy(&out, &back, sizeof(out));
        if (out != b16) {
            /* +0 and -0 are distinct bit patterns and must both survive */
            FAILm("half round trip mismatch");
        }
    }
    PASS();
}

TEST half_known_values(void)
{
    ASSERT_EQ(1.0f, HalfToFloat4(Float4ToHalfUnchecked(1.0f)));
    ASSERT_EQ(-2.5f, HalfToFloat4(Float4ToHalfUnchecked(-2.5f)));
    ASSERT_EQ(65504.0f, HalfToFloat4(Float4ToHalfUnchecked(65504.0f)));
    ASSERT(HalfIsInf(Float4ToHalfUnchecked(1e6f)));
    PASS();
}

static uint64 scalar_hamming(const unsigned char *a, const unsigned char *b, uint32 n)
{
    uint64 d = 0;
    for (uint32 i = 0; i < n; i++)
        d += (uint64) __builtin_popcount((unsigned) (a[i] ^ b[i]));
    return d;
}

TEST bit_hamming_and_jaccard_vs_scalar(void)
{
    BitvecInit();
    unsigned int seed = 12345;
    for (int iter = 0; iter < 500; iter++) {
        uint32 n = (uint32) (rand_r(&seed) % 300) + 1;
        unsigned char a[300], b[300];
        for (uint32 i = 0; i < n; i++) {
            a[i] = (unsigned char) rand_r(&seed);
            b[i] = (unsigned char) rand_r(&seed);
        }
        ASSERT_EQ(scalar_hamming(a, b, n), BitHammingDistance(n, a, b, 0));
        uint64 ab = 0, aa = 0, bb = 0;
        for (uint32 i = 0; i < n; i++) {
            ab += (uint64) __builtin_popcount((unsigned) (a[i] & b[i]));
            aa += (uint64) __builtin_popcount(a[i]);
            bb += (uint64) __builtin_popcount(b[i]);
        }
        double expect = (aa + bb == 0) ? 0.0 : 1.0 - ((double) ab / (double) (aa + bb - ab));
        double got = BitJaccardDistance(n, a, b, 0, 0, 0);
        ASSERT_IN_RANGE(expect, got, 1e-12);
    }
    PASS();
}

TEST halfvec_l2_default_matches_scalar(void)
{
    HalfvecInit();
    half a[37], b[37];
    double ref = 0;
    for (int i = 0; i < 37; i++) {
        a[i] = Float4ToHalfUnchecked((float) i * 0.25f);
        b[i] = Float4ToHalfUnchecked((float) (37 - i) * 0.125f);
        double d = (double) HalfToFloat4(a[i]) - (double) HalfToFloat4(b[i]);
        ref += d * d;
    }
    ASSERT_IN_RANGE(ref, (double) HalfvecL2SquaredDistance(37, a, b), ref * 1e-5);
    PASS();
}

SUITE(kernels)
{
    RUN_TEST(half_roundtrip_all_patterns);
    RUN_TEST(half_known_values);
    RUN_TEST(bit_hamming_and_jaccard_vs_scalar);
    RUN_TEST(halfvec_l2_default_matches_scalar);
}
