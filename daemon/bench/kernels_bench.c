/*
 * kernels_bench.c: micro-benchmark of the pgvector distance kernels as the daemon calls them
 * (PGFunction through DirectFunctionCall2), per CPU tier, type, metric and dimension.
 * Reports median ns/op over repeats. Two working sets:
 *   pool  N random vectors (default 8192, far larger than L2), the HNSW-like cold-ish case
 *   hot   16 vectors, cache resident, isolates compute + call overhead
 * Plus call-path rows: DirectFunctionCall2 vs a pre-built fcinfo call vs a no-op function.
 * Usage: av_kernels_bench [--pairs N] [--repeats R] [--iters I]
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "halfutils.h"
#include "halfvec.h"
#include "kernels/dispatch.h"
#include "utils/memutils.h"
#include "vector.h"

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

static unsigned int rng = 12345;
static double frand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (double) (rng >> 8) / (double) (1u << 24) * 2.0 - 1.0;
}

static volatile double sink;
static int g_repeats = 7;
static long g_iters = 200000;

typedef enum { MODE_DFC, MODE_FCINFO, MODE_NOOP } call_mode;

static Datum noop_fn(FunctionCallInfo fcinfo)
{
    (void) fcinfo;
    return Float8GetDatum(0.0);
}

/* Median ns/op for calling fn over n objects (pairs (i, i+1 mod n)). */
static double bench(av_pgfn fn, void **objs, size_t n, call_mode mode)
{
    double res[32];
    PGFunction pf = mode == MODE_NOOP ? noop_fn : (PGFunction) fn;
    FunctionCallInfoBaseData fc;
    memset(&fc, 0, sizeof fc);
    InitFunctionCallInfoData(fc, NULL, 2, InvalidOid, NULL, NULL);
    for (int r = -1; r < g_repeats; r++) { /* r == -1 is the warm-up */
        double acc = 0;
        size_t i = 0;
        double t0 = now_ns();
        for (long it = 0; it < g_iters; it++) {
            size_t j = i + 1 == n ? 0 : i + 1;
            Datum d;
            if (mode == MODE_FCINFO) {
                fc.args[0].value = PointerGetDatum(objs[i]);
                fc.args[1].value = PointerGetDatum(objs[j]);
                fc.isnull = false;
                d = pf(&fc);
            } else {
                d = DirectFunctionCall2(pf, PointerGetDatum(objs[i]), PointerGetDatum(objs[j]));
            }
            acc += DatumGetFloat8(d);
            i = j;
        }
        double t1 = now_ns();
        sink = acc;
        if (r >= 0)
            res[r] = (t1 - t0) / (double) g_iters;
    }
    qsort(res, (size_t) g_repeats, sizeof res[0], cmp_d);
    return res[g_repeats / 2];
}

#if defined(__x86_64__)
#include <immintrin.h>
/*
 * PROTOTYPE only (not production, not pgvector): the same F16C l2sq as halfutils.c but with 4
 * independent FMA accumulators to hide the 4-cycle FMA latency. Summation order differs, so results
 * differ from pgvector in the last bits. Requires dim % 32 == 0 here (1536, 3072).
 */
__attribute__((target("avx,f16c,fma"))) static float proto_l2sq_4acc(int dim, half *ax, half *bx)
{
    __m256 d0 = _mm256_setzero_ps(), d1 = d0, d2 = d0, d3 = d0;
    for (int i = 0; i < dim; i += 32) {
        __m256 x0 = _mm256_sub_ps(_mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (ax + i))),
                                  _mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (bx + i))));
        __m256 x1 = _mm256_sub_ps(_mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (ax + i + 8))),
                                  _mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (bx + i + 8))));
        __m256 x2 = _mm256_sub_ps(_mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (ax + i + 16))),
                                  _mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (bx + i + 16))));
        __m256 x3 = _mm256_sub_ps(_mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (ax + i + 24))),
                                  _mm256_cvtph_ps(_mm_loadu_si128((__m128i *) (bx + i + 24))));
        d0 = _mm256_fmadd_ps(x0, x0, d0);
        d1 = _mm256_fmadd_ps(x1, x1, d1);
        d2 = _mm256_fmadd_ps(x2, x2, d2);
        d3 = _mm256_fmadd_ps(x3, x3, d3);
    }
    d0 = _mm256_add_ps(_mm256_add_ps(d0, d1), _mm256_add_ps(d2, d3));
    float s[8];
    _mm256_storeu_ps(s, d0);
    return s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];
}
#endif

/* Raw kernel through pgvector function pointer (no fmgr, no CheckDims), hot set. */
static double bench_raw_half(void **objs, size_t n, int dim, int use_proto)
{
    double res[32];
    for (int r = -1; r < g_repeats; r++) {
        double acc = 0;
        size_t i = 0;
        double t0 = now_ns();
        for (long it = 0; it < g_iters; it++) {
            size_t j = i + 1 == n ? 0 : i + 1;
            acc += (double) (use_proto ? proto_l2sq_4acc : HalfvecL2SquaredDistance)(dim, ((HalfVector *) objs[i])->x, ((HalfVector *) objs[j])->x);
            i = j;
        }
        double t1 = now_ns();
        sink = acc;
        if (r >= 0)
            res[r] = (t1 - t0) / (double) g_iters;
    }
    qsort(res, (size_t) g_repeats, sizeof res[0], cmp_d);
    return res[g_repeats / 2];
}

static void **make_pool(av_vtype t, int dim, size_t n)
{
    void **p = palloc(sizeof(void *) * n);
    for (size_t k = 0; k < n; k++) {
        if (t == AV_VT_VECTOR) {
            Vector *v = InitVector(dim);
            for (int i = 0; i < dim; i++)
                v->x[i] = (float) frand();
            p[k] = v;
        } else {
            HalfVector *v = InitHalfVector(dim);
            for (int i = 0; i < dim; i++)
                v->x[i] = Float4ToHalfUnchecked((float) frand());
            p[k] = v;
        }
    }
    return p;
}

int main(int argc, char **argv)
{
    size_t npairs = 8192;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pairs") && i + 1 < argc)
            npairs = (size_t) atol(argv[++i]);
        else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) {
            g_repeats = atoi(argv[++i]);
            if (g_repeats > 31)
                g_repeats = 31;
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            g_iters = atol(argv[++i]);
    }
    av_kernels_init();
    MemoryContext ctx = AllocSetContextCreate(NULL, "bench", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(ctx);

    const char *tiers[] = {"x86-64", "x86-64-v3", "x86-64-v4", "native", "aarch64"};
    const int dims[] = {1536, 3072};
    const av_vtype types[] = {AV_VT_VECTOR, AV_VT_HALFVEC};
    const av_metric metrics[] = {AV_METRIC_L2SQ, AV_METRIC_NEG_IP, AV_METRIC_COSINE};
    const char *mnames[] = {"l2sq", "neg_ip", "cosine"};

    printf("active tier: %s, highest supported: %s, pool: %zu vectors, iters: %ld x %d repeats (median)\n",
           av_kernel_tier(), av_kernel_tier_supported(), npairs, g_iters, g_repeats);
    printf("\n| tier | type | dim | metric | pool ns/op | hot ns/op |\n|---|---|---:|---|---:|---:|\n");
    for (size_t di = 0; di < 2; di++)
        for (size_t ti = 0; ti < 2; ti++) {
            void **pool = make_pool(types[ti], dims[di], npairs);
            void **hot = make_pool(types[ti], dims[di], 16);
            for (size_t k = 0; k < sizeof tiers / sizeof *tiers; k++)
                for (size_t m = 0; m < 3; m++) {
                    av_pgfn fn = av_kernel_fn_tier(tiers[k], types[ti], metrics[m]);
                    if (!fn)
                        continue;
                    double a = bench(fn, pool, npairs, MODE_DFC);
                    double b = bench(fn, hot, 16, MODE_DFC);
                    printf("| %s | %s | %d | %s | %.1f | %.1f |\n", tiers[k],
                           types[ti] == AV_VT_VECTOR ? "vector" : "halfvec", dims[di], mnames[m], a, b);
                    fflush(stdout);
                }
        }

    printf("\n| call path (hot, tier %s) | ns/op |\n|---|---:|\n", av_kernel_tier());
    void **h = make_pool(AV_VT_HALFVEC, 1536, 16);
    av_pgfn f = av_kernel_fn(AV_VT_HALFVEC, AV_METRIC_L2SQ);
    printf("| no-op via DirectFunctionCall2 | %.1f |\n", bench(f, h, 16, MODE_NOOP));
    printf("| halfvec 1536 l2sq via DirectFunctionCall2 | %.1f |\n", bench(f, h, 16, MODE_DFC));
    printf("| halfvec 1536 l2sq via prebuilt fcinfo | %.1f |\n", bench(f, h, 16, MODE_FCINFO));
    printf("| halfvec 1536 l2sq raw kernel pointer (HalfvecL2SquaredDistance) | %.1f |\n", bench_raw_half(h, 16, 1536, 0));
    printf("| halfvec 1536 l2sq PROTOTYPE 4-accumulator F16C (bench-only) | %.1f |\n", bench_raw_half(h, 16, 1536, 1));
    return 0;
}
