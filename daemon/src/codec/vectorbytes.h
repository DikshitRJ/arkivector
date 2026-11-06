/*
 * codec/vectorbytes.h: VectorBytes v1 codec (docs/api/vectors.md section 3).
 *
 * VectorBytes is pgvector's in-memory struct layout with the 4-byte varlena header
 * replaced by { 'A', 'V', version<<4 | type, flags }. Decoding yields a pgvector
 * struct (Vector *, HalfVector *, VarBit *, SparseVector *) allocated in an arena.
 *
 * Untrusted input: av_vb_validate() is the only parser of VectorBytes. It checks in
 * the exact order of spec/error-codes.json (vectorbytes_check_order), reports the
 * first failure and never reads outside [buf, buf + len).
 *
 * Ownership: av_vb_decode() and av_vb_encode() return memory owned by the arena
 * passed in. Nothing here allocates with malloc. All functions are thread-safe
 * (no shared state); an arena must not be shared between threads.
 *
 * Error reporting: on failure the functions return AV_ERR and, when e is not NULL,
 * set e->code = "invalid_vector" and e->vb_error = "VB_*". A NULL e is allowed.
 */
#ifndef AV_CODEC_VECTORBYTES_H
#define AV_CODEC_VECTORBYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec/vtype.h"
#include "core/arena.h"
#include "log.h"

/* NORMALIZED is informational only: on-chain data is untrusted, so consumers never skip
 * normalisation because of it (cosine values are always re-normalised at ingest). */
#define AV_VB_FLAG_NORMALIZED 0x01u

typedef struct {
    av_vtype type;
    uint8_t flags; /* byte 3 of the blob (only bit 0 can be set in a valid blob) */
    int32_t dim;   /* vector/halfvec dim, bit length in bits, sparsevec dim */
    int32_t nnz;   /* sparsevec non-zero count, 0 for other types */
} av_vb_info;

/*
 * Exact VectorBytes length for a type. dim is the dimension (bit length for bit),
 * nnz is used by sparsevec only. Returns false when dim/nnz are outside the limits of
 * the type (vector/halfvec 1..16000, bit 1..64000, sparsevec dim 1..1e9 and
 * 0 <= nnz <= min(dim, 16000)). The size cannot overflow size_t within those limits.
 */
bool av_vb_encoded_size(av_vtype t, int64_t dim, int64_t nnz, size_t *size);

/*
 * Validate a blob. info may be NULL. On success *info is filled. On failure *info
 * is left untouched.
 */
av_status av_vb_validate(const uint8_t *buf, size_t len, av_vb_info *info, av_err *e);

/*
 * Validate and decode into a pgvector struct (allocated 8-byte aligned in a; the
 * varlena header is set with SET_VARSIZE). info may be NULL. *pgvec is set to the
 * struct on success. An allocation failure gives code "internal".
 */
av_status av_vb_decode(const uint8_t *buf, size_t len, av_arena *a, void **pgvec, av_vb_info *info,
                       av_err *e);

/*
 * Encode a pgvector struct (varlena with a valid 4-byte header) of type t. normalized
 * sets the NORMALIZED flag. The produced blob is validated before it is returned, so
 * a struct that holds NaN/Inf values or violates pgvector limits is rejected with
 * the matching VB_* code. *out is arena memory, *len its length.
 */
av_status av_vb_encode(const void *pgvec, av_vtype t, bool normalized, av_arena *a, uint8_t **out,
                       size_t *len, av_err *e);

#endif /* AV_CODEC_VECTORBYTES_H */
