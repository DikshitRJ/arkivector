/*
 * kernels/dispatch.c: x86-64 tier detection and the per-tier kernel tables.
 *
 * CPU feature detection follows pgvector v0.8.7 (src/halfutils.c SupportsCpuFeature
 * and src/bitutils.c SupportsAvx512Popcount): CPUID leaf 1/7 feature bits plus the
 * OS XSAVE state from _xgetbv(0) (& 6 for YMM, & 0xe6 for ZMM). The tier checks add
 * the remaining x86-64-v3/v4 feature bits (psABI levels).
 */
#include "kernels/dispatch.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

typedef av_pgfn kernel_table[5][AV_METRIC_COUNT]; /* [vtype][metric] */

/* pgvector functions (Datum fn(PG_FUNCTION_ARGS)); declared with our opaque types. */
#define DECL(name) extern unsigned long name(av_fcinfo fcinfo)
#define DECL_TIERED(name) DECL(name); DECL(name##_t3); DECL(name##_t4)

DECL_TIERED(l2_distance);
DECL_TIERED(vector_l2_squared_distance);
DECL_TIERED(inner_product);
DECL_TIERED(vector_negative_inner_product);
DECL_TIERED(cosine_distance);
DECL_TIERED(l1_distance);
DECL_TIERED(halfvec_l2_distance);
DECL_TIERED(halfvec_l2_squared_distance);
DECL_TIERED(halfvec_inner_product);
DECL_TIERED(halfvec_negative_inner_product);
DECL_TIERED(halfvec_cosine_distance);
DECL_TIERED(halfvec_l1_distance);
DECL_TIERED(sparsevec_l2_distance);
DECL_TIERED(sparsevec_l2_squared_distance);
DECL_TIERED(sparsevec_inner_product);
DECL_TIERED(sparsevec_negative_inner_product);
DECL_TIERED(sparsevec_cosine_distance);
DECL_TIERED(sparsevec_l1_distance);
DECL(hamming_distance);
DECL(jaccard_distance);
DECL(vector_norm);
DECL(halfvec_l2_norm);
DECL(sparsevec_l2_norm);

#define FILL(tab, sfx)                                                                    \
    do {                                                                                  \
        (tab)[AV_VT_VECTOR][AV_METRIC_L2] = l2_distance##sfx;                             \
        (tab)[AV_VT_VECTOR][AV_METRIC_L2SQ] = vector_l2_squared_distance##sfx;            \
        (tab)[AV_VT_VECTOR][AV_METRIC_IP] = inner_product##sfx;                           \
        (tab)[AV_VT_VECTOR][AV_METRIC_NEG_IP] = vector_negative_inner_product##sfx;       \
        (tab)[AV_VT_VECTOR][AV_METRIC_COSINE] = cosine_distance##sfx;                     \
        (tab)[AV_VT_VECTOR][AV_METRIC_L1] = l1_distance##sfx;                             \
        (tab)[AV_VT_HALFVEC][AV_METRIC_L2] = halfvec_l2_distance##sfx;                    \
        (tab)[AV_VT_HALFVEC][AV_METRIC_L2SQ] = halfvec_l2_squared_distance##sfx;          \
        (tab)[AV_VT_HALFVEC][AV_METRIC_IP] = halfvec_inner_product##sfx;                  \
        (tab)[AV_VT_HALFVEC][AV_METRIC_NEG_IP] = halfvec_negative_inner_product##sfx;     \
        (tab)[AV_VT_HALFVEC][AV_METRIC_COSINE] = halfvec_cosine_distance##sfx;            \
        (tab)[AV_VT_HALFVEC][AV_METRIC_L1] = halfvec_l1_distance##sfx;                    \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_L2] = sparsevec_l2_distance##sfx;                \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_L2SQ] = sparsevec_l2_squared_distance##sfx;      \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_IP] = sparsevec_inner_product##sfx;              \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_NEG_IP] = sparsevec_negative_inner_product##sfx; \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_COSINE] = sparsevec_cosine_distance##sfx;        \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_L1] = sparsevec_l1_distance##sfx;                \
        (tab)[AV_VT_BIT][AV_METRIC_HAMMING] = hamming_distance;                           \
        (tab)[AV_VT_BIT][AV_METRIC_JACCARD] = jaccard_distance;                           \
        (tab)[AV_VT_VECTOR][AV_METRIC_NORM] = vector_norm;                                \
        (tab)[AV_VT_HALFVEC][AV_METRIC_NORM] = halfvec_l2_norm;                           \
        (tab)[AV_VT_SPARSEVEC][AV_METRIC_NORM] = sparsevec_l2_norm;                       \
    } while (0)

static kernel_table tab_base;
static kernel_table tab_t3;
static kernel_table tab_t4;
static kernel_table *active = &tab_base;
static const char *active_name = "generic";
static const char *supported_name = "generic";
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* pgvector halfutils.c / bitutils.c dispatch init (function pointers). */
extern void HalfvecInit(void);
extern void BitvecInit(void);

#if defined(__x86_64__)
#define CPU_FEATURE_FMA (1u << 12)
#define CPU_FEATURE_MOVBE (1u << 22)
#define CPU_FEATURE_POPCNT (1u << 23)
#define CPU_FEATURE_OSXSAVE (1u << 27)
#define CPU_FEATURE_AVX (1u << 28)
#define CPU_FEATURE_F16C (1u << 29)
#define CPU_FEATURE7_BMI1 (1u << 3)
#define CPU_FEATURE7_AVX2 (1u << 5)
#define CPU_FEATURE7_BMI2 (1u << 8)
#define CPU_FEATURE7_AVX512F (1u << 16)
#define CPU_FEATURE7_AVX512DQ (1u << 17)
#define CPU_FEATURE7_AVX512CD (1u << 28)
#define CPU_FEATURE7_AVX512BW (1u << 30)
#define CPU_FEATURE7_AVX512VL (1u << 31)
#define CPU_FEATUREX_LZCNT (1u << 5)

__attribute__((target("xsave"))) static unsigned long long os_xstate(void)
{
    return (unsigned long long) _xgetbv(0);
}

static int detect_level(void)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) {
        return 1;
    }
    unsigned int ecx1 = c;
    /* Check OS supports XSAVE (pgvector halfutils.c) */
    if ((ecx1 & CPU_FEATURE_OSXSAVE) != CPU_FEATURE_OSXSAVE) {
        return 1;
    }
    unsigned long long xcr0 = os_xstate();
    unsigned int ebx7 = 0;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) {
        ebx7 = b;
    }
    unsigned int ecxx = 0;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) {
        ecxx = c;
    }
    const unsigned int v3_1 = CPU_FEATURE_FMA | CPU_FEATURE_MOVBE | CPU_FEATURE_POPCNT | CPU_FEATURE_AVX |
                              CPU_FEATURE_F16C;
    const unsigned int v3_7 = CPU_FEATURE7_BMI1 | CPU_FEATURE7_AVX2 | CPU_FEATURE7_BMI2;
    /* XMM and YMM registers enabled (pgvector halfutils.c: _xgetbv(0) & 6) */
    bool v3 = (ecx1 & v3_1) == v3_1 && (ebx7 & v3_7) == v3_7 && (ecxx & CPU_FEATUREX_LZCNT) &&
              (xcr0 & 6) == 6;
    if (!v3) {
        return 1;
    }
    const unsigned int v4_7 = CPU_FEATURE7_AVX512F | CPU_FEATURE7_AVX512DQ | CPU_FEATURE7_AVX512CD |
                              CPU_FEATURE7_AVX512BW | CPU_FEATURE7_AVX512VL;
    /* opmask, upper ZMM and ZMM16-31 enabled (pgvector bitutils.c: _xgetbv(0) & 0xe6) */
    bool v4 = (ebx7 & v4_7) == v4_7 && (xcr0 & 0xe6) == 0xe6;
    return v4 ? 4 : 3;
}
#endif

static void init_once(void)
{
    HalfvecInit();
    BitvecInit();
    FILL(tab_base, );
#if defined(AV_KERNEL_TIERS) && defined(__x86_64__)
    FILL(tab_t3, _t3);
    FILL(tab_t4, _t4);
    int level = detect_level();
    supported_name = level == 4 ? "x86-64-v4" : level == 3 ? "x86-64-v3" : "x86-64";
    const char *env = getenv("AV_KERNEL_TIER");
    if (env != NULL && strcmp(env, "x86-64") == 0) {
        level = 1;
    } else if (env != NULL && strcmp(env, "x86-64-v3") == 0 && level >= 3) {
        level = 3;
    }
    active = level == 4 ? &tab_t4 : level == 3 ? &tab_t3 : &tab_base;
    active_name = level == 4 ? "x86-64-v4" : level == 3 ? "x86-64-v3" : "x86-64";
#else
    memcpy(tab_t3, tab_base, sizeof(tab_base));
    memcpy(tab_t4, tab_base, sizeof(tab_base));
#if defined(__aarch64__)
    active_name = supported_name = "aarch64";
#elif defined(__x86_64__)
    active_name = supported_name = "native";
#endif
#endif
}

void av_kernels_init(void)
{
    pthread_once(&once, init_once);
}

const char *av_kernel_tier(void)
{
    av_kernels_init();
    return active_name;
}

const char *av_kernel_tier_supported(void)
{
    av_kernels_init();
    return supported_name;
}

av_pgfn av_kernel_fn(av_vtype type, av_metric metric)
{
    av_kernels_init();
    if ((int) type < 1 || (int) type > 4 || (int) metric < 0 || metric >= AV_METRIC_COUNT) {
        return NULL;
    }
    return (*active)[type][metric];
}

av_pgfn av_kernel_fn_tier(const char *tier, av_vtype type, av_metric metric)
{
    av_kernels_init();
    if ((int) type < 1 || (int) type > 4 || (int) metric < 0 || metric >= AV_METRIC_COUNT) {
        return NULL;
    }
    const char *sup = supported_name;
    if (strcmp(tier, "x86-64") == 0 || strcmp(tier, "generic") == 0 || strcmp(tier, sup) == 0 ||
        strcmp(tier, "native") == 0 || strcmp(tier, "aarch64") == 0) {
        if (strcmp(tier, "x86-64-v4") == 0) {
            return tab_t4[type][metric];
        }
        if (strcmp(tier, "x86-64-v3") == 0) {
            return tab_t3[type][metric];
        }
        return tab_base[type][metric];
    }
    if (strcmp(tier, "x86-64-v3") == 0 && strcmp(sup, "x86-64-v4") == 0) {
        return tab_t3[type][metric];
    }
    return NULL;
}
