/*
 * kernels/opclass.h: registry of the 14 pgvector operator classes
 * (spec/openapi.yaml#/components/schemas/Opclass), mapped exactly as in
 * pgvector v0.8.7 sql/vector.sql:
 *   distance       = the SQL operator's function (what the API returns)
 *   index_distance = HNSW support FUNCTION 1 (l2 squared, negative inner product, ...)
 *   normalize      = the opclass has a FUNCTION 2 norm proc (cosine): values are
 *                    l2-normalised before insertion and for queries (HnswNormValue)
 * Distance functions go through the dispatched kernels (kernels/dispatch.h). They
 * never raise: on a dimension mismatch they return NaN (validate dims first).
 */
#ifndef AV_KERNELS_OPCLASS_H
#define AV_KERNELS_OPCLASS_H

#include <stdbool.h>
#include <stddef.h>

#include "codec/vtype.h"
#include "kernels/dispatch.h"

typedef double (*av_dist_fn)(const void *a, const void *b); /* pgvector struct pointers */

typedef struct {
    const char *name; /* "halfvec_cosine_ops" */
    av_vtype type;
    const char *op;            /* "<=>" */
    av_dist_fn distance;       /* API-facing distance (sqrt applied for l2) */
    av_dist_fn index_distance; /* distance used inside HNSW */
    bool normalize;            /* cosine: l2-normalise at insert and query */
    av_metric metric;          /* metric of distance */
    av_metric index_metric;    /* metric of index_distance */
} av_opclass;

const av_opclass *av_opclass_find(const char *name);
const av_opclass *av_opclass_for_operator(av_vtype t, const char *op);
const av_opclass *av_opclass_at(size_t i); /* NULL past the end */
size_t av_opclass_count(void);

/* The pgvector SQL function behind index_distance / the norm proc (for HNSW support). */
av_pgfn av_opclass_index_pgfn(const av_opclass *oc);
av_pgfn av_opclass_norm_pgfn(const av_opclass *oc); /* NULL unless normalize */

/* "x86-64" | "x86-64-v3" | "x86-64-v4" | "aarch64" | "native" */
const char *av_kernel_tier(void);

#endif /* AV_KERNELS_OPCLASS_H */
