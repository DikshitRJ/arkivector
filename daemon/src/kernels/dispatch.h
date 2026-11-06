/*
 * kernels/dispatch.h: CPU tier detection and the table of pgvector distance
 * functions (PGFunction, called through fmgr) per vector type and metric.
 *
 * On x86-64 the kernel files are compiled for baseline, x86-64-v3 and x86-64-v4
 * (cmake/KernelTiers.cmake); av_kernels_init() picks the best supported tier once
 * at startup. Call it before any search or index build; it is idempotent and
 * thread-safe (pthread_once). Override with env AV_KERNEL_TIER=x86-64|x86-64-v3|x86-64-v4
 * (only to a tier the CPU supports).
 */
#ifndef AV_KERNELS_DISPATCH_H
#define AV_KERNELS_DISPATCH_H

#include <stdbool.h>

#include "codec/vtype.h"

typedef enum {
    AV_METRIC_L2 = 0,  /* <->, sqrt applied */
    AV_METRIC_L2SQ,    /* squared L2 (HNSW index distance) */
    AV_METRIC_IP,      /* inner product (positive) */
    AV_METRIC_NEG_IP,  /* <#>, negative inner product */
    AV_METRIC_COSINE,  /* <=> */
    AV_METRIC_L1,      /* <+> */
    AV_METRIC_HAMMING, /* <~> (bit) */
    AV_METRIC_JACCARD, /* <%> (bit) */
    AV_METRIC_NORM,    /* l2 norm of one vector (cosine normalisation check) */
    AV_METRIC_COUNT
} av_metric;

/* Opaque pgvector SQL-callable function: Datum fn(FunctionCallInfo). */
typedef struct FunctionCallInfoBaseData *av_fcinfo;
typedef unsigned long (*av_pgfn)(av_fcinfo fcinfo);

void av_kernels_init(void);

/* "x86-64" | "x86-64-v3" | "x86-64-v4" | "aarch64" | "native" | "generic". */
const char *av_kernel_tier(void);

/* Highest tier the CPU/OS supports (independent of the env override). */
const char *av_kernel_tier_supported(void);

/* Function for (type, metric), NULL when pgvector has none for that pair. */
av_pgfn av_kernel_fn(av_vtype type, av_metric metric);

/* Same, for an explicitly named tier (tests). NULL if unknown/unsupported. */
av_pgfn av_kernel_fn_tier(const char *tier, av_vtype type, av_metric metric);

#endif /* AV_KERNELS_DISPATCH_H */
