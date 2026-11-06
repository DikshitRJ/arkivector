/*
 * av_tier.h: ARKIVECTOR-owned. Force-included (-include) when a pgvector kernel file
 * is compiled for an x86-64 tier, so the copied file stays unmodified while its
 * exported SQL-callable functions get a tier suffix (AV_TIER_SUFFIX, e.g. _t3).
 * Static helpers need no renaming. See cmake/KernelTiers.cmake, kernels/dispatch.c.
 */
#ifndef AV_TIER_H
#define AV_TIER_H
#ifdef AV_TIER_SUFFIX
#define AV_TIER_CAT2(a, b) a##b
#define AV_TIER_CAT(a, b) AV_TIER_CAT2(a, b)
#define AV_TIER_NAME(n) AV_TIER_CAT(n, AV_TIER_SUFFIX)

#define l2_distance AV_TIER_NAME(l2_distance)
#define vector_l2_squared_distance AV_TIER_NAME(vector_l2_squared_distance)
#define inner_product AV_TIER_NAME(inner_product)
#define vector_negative_inner_product AV_TIER_NAME(vector_negative_inner_product)
#define cosine_distance AV_TIER_NAME(cosine_distance)
#define l1_distance AV_TIER_NAME(l1_distance)

#define halfvec_l2_distance AV_TIER_NAME(halfvec_l2_distance)
#define halfvec_l2_squared_distance AV_TIER_NAME(halfvec_l2_squared_distance)
#define halfvec_inner_product AV_TIER_NAME(halfvec_inner_product)
#define halfvec_negative_inner_product AV_TIER_NAME(halfvec_negative_inner_product)
#define halfvec_cosine_distance AV_TIER_NAME(halfvec_cosine_distance)
#define halfvec_l1_distance AV_TIER_NAME(halfvec_l1_distance)

#define sparsevec_l2_distance AV_TIER_NAME(sparsevec_l2_distance)
#define sparsevec_l2_squared_distance AV_TIER_NAME(sparsevec_l2_squared_distance)
#define sparsevec_inner_product AV_TIER_NAME(sparsevec_inner_product)
#define sparsevec_negative_inner_product AV_TIER_NAME(sparsevec_negative_inner_product)
#define sparsevec_cosine_distance AV_TIER_NAME(sparsevec_cosine_distance)
#define sparsevec_l1_distance AV_TIER_NAME(sparsevec_l1_distance)
#endif
#endif /* AV_TIER_H */
