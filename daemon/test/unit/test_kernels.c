/*
 * test_kernels.c (D0 part): pgvector half-float conversion and bit distance kernels.
 */
#include "postgres.h"

#include "bitutils.h"
#include "bitvec.h"
#include "halfutils.h"
#include "halfvec.h"
#include "port/pg_bitutils.h"
#include "sparsevec.h"
#include "utils/memutils.h"
#include "utils/varbit.h"
#include "varatt.h"
#include "vector.h"

#include "kernels/dispatch.h"
#include "kernels/opclass.h"
#include "codec/text.h"

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

/* ---- multi-tier kernels vs double-precision reference (PLAN_DAEMON D1 step 8) ---- */

static const int test_dims[] = {1, 3, 8, 15, 16, 17, 31, 32, 33, 1536, 3072, 16000};

static double frand(unsigned int *seed)
{
    return (double) rand_r(seed) / (double) RAND_MAX * 2.0 - 1.0;
}

/* Condition scale for float32 accumulation: sum of |terms| (inner products cancel). */
static double g_ip_abs;

static double ref_metric(av_metric m, const double *a, const double *b, int n)
{
    double dot = 0, na = 0, nb = 0, l2 = 0, l1 = 0;
    g_ip_abs = 0;
    for (int i = 0; i < n; i++) {
        g_ip_abs += fabs(a[i] * b[i]);
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
        l2 += (a[i] - b[i]) * (a[i] - b[i]);
        l1 += fabs(a[i] - b[i]);
    }
    switch (m) {
    case AV_METRIC_L2:
        return sqrt(l2);
    case AV_METRIC_L2SQ:
        return l2;
    case AV_METRIC_IP:
        return dot;
    case AV_METRIC_NEG_IP:
        return -dot;
    case AV_METRIC_COSINE: {
        double sim = dot / sqrt(na * nb);
        if (sim > 1)
            sim = 1;
        if (sim < -1)
            sim = -1;
        return 1 - sim;
    }
    case AV_METRIC_L1:
        return l1;
    default:
        return NAN;
    }
}

static const av_metric float_metrics[] = {AV_METRIC_L2,     AV_METRIC_L2SQ,   AV_METRIC_IP,
                                          AV_METRIC_NEG_IP, AV_METRIC_COSINE, AV_METRIC_L1};

static double call_fn(av_pgfn fn, const void *a, const void *b)
{
    return DatumGetFloat8(DirectFunctionCall2((PGFunction) fn, PointerGetDatum(a), PointerGetDatum(b)));
}

static int check_close(double got, double ref, double rel, double scale)
{
    double tol = rel * fmax(fabs(ref), scale);
    return fabs(got - ref) <= tol;
}

TEST tiered_float_kernels_match_reference(void)
{
    const char *tiers[] = {"x86-64", "x86-64-v3", "x86-64-v4", "native", "aarch64"};
    MemoryContext ctx = AllocSetContextCreate(NULL, "k", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    unsigned int seed = 99;
    int tested_tiers = 0;
    for (size_t ti = 0; ti < sizeof(tiers) / sizeof(tiers[0]); ti++) {
        if (av_kernel_fn_tier(tiers[ti], AV_VT_VECTOR, AV_METRIC_L2) == NULL)
            continue;
        tested_tiers++;
        for (size_t di = 0; di < sizeof(test_dims) / sizeof(test_dims[0]); di++) {
            int n = test_dims[di];
            double *ra = palloc(sizeof(double) * (size_t) n), *rb = palloc(sizeof(double) * (size_t) n);
            double *ha = palloc(sizeof(double) * (size_t) n), *hb = palloc(sizeof(double) * (size_t) n);
            Vector *va = InitVector(n), *vb = InitVector(n);
            HalfVector *qa = InitHalfVector(n), *qb = InitHalfVector(n);
            for (int i = 0; i < n; i++) {
                va->x[i] = (float) frand(&seed);
                vb->x[i] = (float) frand(&seed);
                ra[i] = va->x[i];
                rb[i] = vb->x[i];
                qa->x[i] = Float4ToHalfUnchecked(va->x[i]);
                qb->x[i] = Float4ToHalfUnchecked(vb->x[i]);
                ha[i] = HalfToFloat4(qa->x[i]);
                hb[i] = HalfToFloat4(qb->x[i]);
            }
            for (size_t mi = 0; mi < sizeof(float_metrics) / sizeof(float_metrics[0]); mi++) {
                av_metric m = float_metrics[mi];
                double rv = ref_metric(m, ra, rb, n);
                double sv = (m == AV_METRIC_IP || m == AV_METRIC_NEG_IP) ? g_ip_abs : 1.0;
                double rh = ref_metric(m, ha, hb, n);
                double sh = (m == AV_METRIC_IP || m == AV_METRIC_NEG_IP) ? g_ip_abs : 1.0;
                double gv = call_fn(av_kernel_fn_tier(tiers[ti], AV_VT_VECTOR, m), va, vb);
                double gh = call_fn(av_kernel_fn_tier(tiers[ti], AV_VT_HALFVEC, m), qa, qb);
                /* float32 accumulation: relative 1e-5 (vector), 2e-3 (halfvec); scale for near-zero */
                if (!check_close(gv, rv, 1e-5, sv)) {
                    fprintf(stderr, "vector tier %s dim %d metric %d: got %.9g ref %.9g\n", tiers[ti], n, m,
                            gv, rv);
                    FAILm("vector kernel mismatch");
                }
                if (!check_close(gh, rh, 2e-3, sh)) {
                    fprintf(stderr, "halfvec tier %s dim %d metric %d: got %.9g ref %.9g\n", tiers[ti], n, m,
                            gh, rh);
                    FAILm("halfvec kernel mismatch");
                }
            }
            /* sparsevec: same values on up to 200 random distinct indices */
            int nnz = n < 200 ? n : 200;
            SparseVector *sa = InitSparseVector(n, nnz), *sb = InitSparseVector(n, nnz);
            double *da = palloc0(sizeof(double) * (size_t) n), *db = palloc0(sizeof(double) * (size_t) n);
            int stride = n / nnz;
            for (int k = 0; k < nnz; k++) {
                int idx = k * stride;
                float x = (float) frand(&seed), y = (float) frand(&seed);
                if (x == 0)
                    x = 0.5f;
                if (y == 0)
                    y = -0.5f;
                sa->indices[k] = idx;
                SPARSEVEC_VALUES(sa)[k] = x;
                da[idx] = x;
                /* b uses shifted indices to exercise the merge */
                int jdx = (stride > 1 && (k % 2)) ? idx + 1 : idx; /* strictly ascending, distinct */
                sb->indices[k] = jdx;
                SPARSEVEC_VALUES(sb)[k] = y;
            }
            for (int k = 0; k < nnz; k++)
                db[sb->indices[k]] = SPARSEVEC_VALUES(sb)[k];
            for (size_t mi = 0; mi < sizeof(float_metrics) / sizeof(float_metrics[0]); mi++) {
                av_metric m = float_metrics[mi];
                double rs = ref_metric(m, da, db, n);
                double ss = (m == AV_METRIC_IP || m == AV_METRIC_NEG_IP) ? g_ip_abs : 1e-2;
                double gs = call_fn(av_kernel_fn_tier(tiers[ti], AV_VT_SPARSEVEC, m), sa, sb);
                if (!check_close(gs, rs, 1e-5, m == AV_METRIC_COSINE ? 1.0 : ss)) {
                    fprintf(stderr, "sparsevec tier %s dim %d metric %d: got %.9g ref %.9g\n", tiers[ti], n,
                            m, gs, rs);
                    FAILm("sparsevec kernel mismatch");
                }
            }
            MemoryContextReset(ctx);
        }
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    fprintf(stderr, "[kernels] active tier %s (supported %s), %d tier(s) tested\n", av_kernel_tier(),
            av_kernel_tier_supported(), tested_tiers);
    ASSERT(tested_tiers >= 1);
    PASS();
}

TEST bit_kernels_exact(void)
{
    MemoryContext ctx = AllocSetContextCreate(NULL, "k", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    unsigned int seed = 5;
    for (size_t di = 0; di < sizeof(test_dims) / sizeof(test_dims[0]); di++) {
        int n = test_dims[di];
        VarBit *a = InitBitVector(n), *b = InitBitVector(n);
        uint64 ham = 0, ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < n; i++) {
            int x = rand_r(&seed) & 1, y = rand_r(&seed) & 1;
            if (x)
                VARBITS(a)[i / 8] |= (bits8) (0x80 >> (i % 8));
            if (y)
                VARBITS(b)[i / 8] |= (bits8) (0x80 >> (i % 8));
            ham += (uint64) (x ^ y);
            ab += (uint64) (x & y);
            aa += (uint64) x;
            bb += (uint64) y;
        }
        double gh = call_fn(av_kernel_fn(AV_VT_BIT, AV_METRIC_HAMMING), a, b);
        double gj = call_fn(av_kernel_fn(AV_VT_BIT, AV_METRIC_JACCARD), a, b);
        ASSERT_EQ((double) ham, gh);
        double rj =
            ab == 0 ? 1.0 : 1.0 - (double) ab / (double) (aa + bb - ab); /* pgvector: no overlap -> 1 */
        ASSERT_IN_RANGE(rj, gj, 1e-12);
        MemoryContextReset(ctx);
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    PASS();
}

TEST opclass_registry_matches_pgvector(void)
{
    ASSERT_EQ(14u, av_opclass_count());
    const av_opclass *oc = av_opclass_find("halfvec_cosine_ops");
    ASSERT(oc != NULL);
    ASSERT_EQ(AV_VT_HALFVEC, oc->type);
    ASSERT_STR_EQ("<=>", oc->op);
    ASSERT(oc->normalize);
    ASSERT_EQ(AV_METRIC_NEG_IP, oc->index_metric);
    ASSERT(av_opclass_norm_pgfn(oc) != NULL);
    ASSERT_EQ(av_opclass_find("vector_l2_ops")->index_metric, AV_METRIC_L2SQ);
    ASSERT_EQ(av_opclass_for_operator(AV_VT_SPARSEVEC, "<+>"), av_opclass_find("sparsevec_l1_ops"));
    ASSERT_EQ(av_opclass_for_operator(AV_VT_BIT, "<%>"), av_opclass_find("bit_jaccard_ops"));
    ASSERT_EQ(NULL, av_opclass_for_operator(AV_VT_BIT, "<->"));
    ASSERT_EQ(NULL, av_opclass_find("vector_foo_ops"));
    for (size_t i = 0; i < av_opclass_count(); i++) {
        const av_opclass *o = av_opclass_at(i);
        ASSERT(o->distance != NULL && o->index_distance != NULL);
        ASSERT(av_opclass_index_pgfn(o) != NULL);
    }
    /* distance thunk equals pgvector's l2_distance on a 3-4-5 triangle */
    MemoryContext ctx = AllocSetContextCreate(NULL, "k", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    Vector *a = InitVector(2), *b = InitVector(2);
    a->x[0] = 0, a->x[1] = 0, b->x[0] = 3, b->x[1] = 4;
    ASSERT_EQ(5.0, av_opclass_find("vector_l2_ops")->distance(a, b));
    ASSERT_EQ(25.0, av_opclass_find("vector_l2_ops")->index_distance(a, b));
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    PASS();
}

TEST hardening_limits(void)
{
    /* opclass distance on mismatched dims: NaN, never an ERROR */
    MemoryContext ctx = AllocSetContextCreate(NULL, "k", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    Vector *a = InitVector(2), *b = InitVector(3);
    ASSERT(isnan(av_opclass_find("vector_l2_ops")->distance(a, b)));
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    /* text literals: NUL byte, oversized bit string */
    av_arena *ar = av_arena_create("t", 0, 0);
    av_err e;
    void *v = NULL;
    ASSERT_EQ(AV_ERR, av_text_to_pgvecn(AV_VT_VECTOR, "[1,2]\0[3]", 9, -1, ar, &v, &e));
    ASSERT_STR_EQ("invalid_vector", e.code);
    size_t n = 64001;
    char *bits = malloc(n + 1);
    memset(bits, '1', n);
    bits[n] = '\0';
    ASSERT_EQ(AV_ERR, av_text_to_pgvec(AV_VT_BIT, bits, -1, ar, &v, &e));
    bits[64000] = '\0';
    ASSERT_EQ(AV_OK, av_text_to_pgvec(AV_VT_BIT, bits, -1, ar, &v, &e));
    free(bits);
    av_arena_destroy(ar);
    PASS();
}

SUITE(kernels)
{
    RUN_TEST(hardening_limits);
    RUN_TEST(tiered_float_kernels_match_reference);
    RUN_TEST(bit_kernels_exact);
    RUN_TEST(opclass_registry_matches_pgvector);
    RUN_TEST(half_roundtrip_all_patterns);
    RUN_TEST(half_known_values);
    RUN_TEST(bit_hamming_and_jaccard_vs_scalar);
    RUN_TEST(halfvec_l2_default_matches_scalar);
}
