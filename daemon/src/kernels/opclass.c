/*
 * kernels/opclass.c: opclass registry. See opclass.h.
 */
#include "kernels/opclass.h"

#include <string.h>

#include "postgres.h"

#include "fmgr.h"

#include <math.h>

/*
 * Never raises: pgvector's CheckDims would ereport(ERROR) on a dimension mismatch, so
 * the dims are compared first and NaN is returned instead. Callers validate query
 * dims at the API boundary (dimension_mismatch); this is the backstop.
 */
static double call2(av_vtype t, av_metric m, const void *a, const void *b)
{
    if (av_pgvec_dim(t, a) != av_pgvec_dim(t, b)) {
        return NAN;
    }
    PGFunction fn = (PGFunction) av_kernel_fn(t, m);
    Datum d = DirectFunctionCall2(fn, PointerGetDatum(a), PointerGetDatum(b));
    return DatumGetFloat8(d);
}

#define THUNK(tname, t, mname, m)                                       \
    static double thunk_##tname##_##mname(const void *a, const void *b) \
    {                                                                   \
        return call2(t, m, a, b);                                       \
    }

#define THUNKS(tname, t)                     \
    THUNK(tname, t, l2, AV_METRIC_L2)        \
    THUNK(tname, t, l2sq, AV_METRIC_L2SQ)    \
    THUNK(tname, t, negip, AV_METRIC_NEG_IP) \
    THUNK(tname, t, cos, AV_METRIC_COSINE)   \
    THUNK(tname, t, l1, AV_METRIC_L1)

THUNKS(vector, AV_VT_VECTOR)
THUNKS(halfvec, AV_VT_HALFVEC)
THUNKS(sparsevec, AV_VT_SPARSEVEC)
THUNK(bit, AV_VT_BIT, hamming, AV_METRIC_HAMMING)
THUNK(bit, AV_VT_BIT, jaccard, AV_METRIC_JACCARD)

#define OC_FLOAT(tname, t)                                                                                   \
    {#tname "_l2_ops",     t,     "<->",        thunk_##tname##_l2,                                          \
     thunk_##tname##_l2sq, false, AV_METRIC_L2, AV_METRIC_L2SQ},                                             \
        {#tname "_ip_ops", t, "<#>", thunk_##tname##_negip, thunk_##tname##_negip, false, AV_METRIC_NEG_IP,  \
         AV_METRIC_NEG_IP},                                                                                  \
        {#tname "_cosine_ops", t, "<=>", thunk_##tname##_cos, thunk_##tname##_negip, true, AV_METRIC_COSINE, \
         AV_METRIC_NEG_IP},                                                                                  \
    {                                                                                                        \
        #tname "_l1_ops", t, "<+>", thunk_##tname##_l1, thunk_##tname##_l1, false, AV_METRIC_L1,             \
            AV_METRIC_L1                                                                                     \
    }

static const av_opclass registry[] = {
    OC_FLOAT(vector, AV_VT_VECTOR),
    OC_FLOAT(halfvec, AV_VT_HALFVEC),
    OC_FLOAT(sparsevec, AV_VT_SPARSEVEC),
    {"bit_hamming_ops", AV_VT_BIT, "<~>", thunk_bit_hamming, thunk_bit_hamming, false, AV_METRIC_HAMMING,
     AV_METRIC_HAMMING},
    {"bit_jaccard_ops", AV_VT_BIT, "<%>", thunk_bit_jaccard, thunk_bit_jaccard, false, AV_METRIC_JACCARD,
     AV_METRIC_JACCARD},
};

#define N_OPCLASS (sizeof(registry) / sizeof(registry[0]))

const av_opclass *av_opclass_find(const char *name)
{
    for (size_t i = 0; i < N_OPCLASS; i++) {
        if (strcmp(registry[i].name, name) == 0) {
            return &registry[i];
        }
    }
    return NULL;
}

const av_opclass *av_opclass_for_operator(av_vtype t, const char *op)
{
    for (size_t i = 0; i < N_OPCLASS; i++) {
        if (registry[i].type == t && strcmp(registry[i].op, op) == 0) {
            return &registry[i];
        }
    }
    return NULL;
}

const av_opclass *av_opclass_at(size_t i)
{
    return i < N_OPCLASS ? &registry[i] : NULL;
}

size_t av_opclass_count(void)
{
    return N_OPCLASS;
}

av_pgfn av_opclass_index_pgfn(const av_opclass *oc)
{
    return av_kernel_fn(oc->type, oc->index_metric);
}

av_pgfn av_opclass_norm_pgfn(const av_opclass *oc)
{
    return oc->normalize ? av_kernel_fn(oc->type, AV_METRIC_NORM) : NULL;
}
