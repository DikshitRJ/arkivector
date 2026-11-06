/*
 * codec/vectorbytes.c: VectorBytes v1 codec (see vectorbytes.h).
 *
 * All multi-byte fields are read with explicit little-endian helpers, so the code
 * does not depend on host endianness or on the alignment of the input buffer.
 * (The struct copy in decode/encode assumes a little-endian host, like pgvector.)
 */
/* pgcompat headers use __int128, which -Wpedantic rejects: relax it for this include only */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "postgres.h"
#include "varatt.h"
#pragma GCC diagnostic pop

#include <string.h>

#include "codec/vectorbytes.h"

#define VB_HDR            4u
#define VB_MAX_DIM_FLOAT  16000
#define VB_MAX_DIM_BIT    64000
#define VB_MAX_DIM_SPARSE 1000000000
#define VB_MAX_NNZ        16000

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p)
{
    uint32_t u = rd_u32(p);
    int32_t v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static int16_t rd_i16(const uint8_t *p)
{
    uint16_t u = rd_u16(p);
    int16_t v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static av_status vb_fail(av_err *e, const char *vb, const char *what)
{
    av_err_set_vb(e, vb, "invalid vector: %s", what);
    return AV_ERR;
}

bool av_vb_encoded_size(av_vtype t, int64_t dim, int64_t nnz, size_t *size)
{
    switch (t) {
    case AV_VT_VECTOR:
        if (dim < 1 || dim > VB_MAX_DIM_FLOAT)
            return false;
        *size = 8u + 4u * (size_t) dim;
        return true;
    case AV_VT_HALFVEC:
        if (dim < 1 || dim > VB_MAX_DIM_FLOAT)
            return false;
        *size = 8u + 2u * (size_t) dim;
        return true;
    case AV_VT_BIT:
        if (dim < 1 || dim > VB_MAX_DIM_BIT)
            return false;
        *size = 8u + ((size_t) dim + 7u) / 8u;
        return true;
    case AV_VT_SPARSEVEC:
        if (dim < 1 || dim > VB_MAX_DIM_SPARSE || nnz < 0 || nnz > VB_MAX_NNZ || nnz > dim)
            return false;
        *size = 16u + 8u * (size_t) nnz;
        return true;
    }
    return false;
}

av_status av_vb_validate(const uint8_t *buf, size_t len, av_vb_info *info, av_err *e)
{
    /* 1. length >= 8 */
    if (buf == NULL || len < 8)
        return vb_fail(e, "VB_TOO_SHORT", "shorter than the 8-byte fixed header");
    /* 2. magic */
    if (buf[0] != 'A' || buf[1] != 'V')
        return vb_fail(e, "VB_BAD_MAGIC", "bytes 0-1 are not 'AV'");
    /* 3. version */
    if ((buf[2] >> 4) != 1)
        return vb_fail(e, "VB_BAD_VERSION", "format version is not 1");
    /* 4. type */
    unsigned tn = buf[2] & 0x0fu;
    if (tn < 1 || tn > 4)
        return vb_fail(e, "VB_UNKNOWN_TYPE", "type nibble is not 1..4");
    av_vtype t = (av_vtype) tn;
    /* 1 (continued). sparsevec needs 16 bytes once the type is known */
    if (t == AV_VT_SPARSEVEC && len < 16)
        return vb_fail(e, "VB_TOO_SHORT", "sparsevec shorter than the 16-byte fixed header");
    /* 5. flags */
    uint8_t flags = buf[3];
    if ((flags & 0xfeu) != 0)
        return vb_fail(e, "VB_BAD_FLAGS", "reserved flag bits are set");

    int64_t dim;
    int64_t nnz = 0;
    if (t == AV_VT_VECTOR || t == AV_VT_HALFVEC) {
        dim = rd_i16(buf + 4);
        /* 6. unused */
        if (rd_u16(buf + 6) != 0)
            return vb_fail(e, "VB_UNUSED_NONZERO", "unused field is not zero");
        /* 7. dim */
        if (dim < 1 || dim > VB_MAX_DIM_FLOAT)
            return vb_fail(e, "VB_DIM_OUT_OF_RANGE", "dim is outside 1..16000");
    } else if (t == AV_VT_BIT) {
        dim = rd_i32(buf + 4);
        if (dim < 1 || dim > VB_MAX_DIM_BIT)
            return vb_fail(e, "VB_DIM_OUT_OF_RANGE", "bit length is outside 1..64000");
    } else {
        dim = rd_i32(buf + 4);
        nnz = rd_i32(buf + 8);
        if (rd_u32(buf + 12) != 0)
            return vb_fail(e, "VB_UNUSED_NONZERO", "unused field is not zero");
        if (dim < 1 || dim > VB_MAX_DIM_SPARSE)
            return vb_fail(e, "VB_DIM_OUT_OF_RANGE", "dim is outside 1..1000000000");
        /* 8. nnz */
        if (nnz < 0 || nnz > dim || nnz > VB_MAX_NNZ)
            return vb_fail(e, "VB_NNZ_OUT_OF_RANGE", "nnz is outside 0..min(dim,16000)");
    }

    /* 9. exact length */
    size_t want = 0;
    if (!av_vb_encoded_size(t, dim, nnz, &want))
        return vb_fail(e, "VB_DIM_OUT_OF_RANGE", "dimension limits"); /* unreachable: limits checked above */
    if (len != want)
        return vb_fail(e, "VB_LENGTH_MISMATCH", "length differs from the length implied by the header");

    /* 10-14. payload checks */
    if (t == AV_VT_VECTOR) {
        for (int64_t i = 0; i < dim; i++) {
            if (((rd_u32(buf + 8 + 4 * i) >> 23) & 0xffu) == 0xffu)
                return vb_fail(e, "VB_NON_FINITE", "a value is NaN or infinite");
        }
    } else if (t == AV_VT_HALFVEC) {
        for (int64_t i = 0; i < dim; i++) {
            if (((rd_u16(buf + 8 + 2 * i) >> 10) & 0x1fu) == 0x1fu)
                return vb_fail(e, "VB_NON_FINITE", "a value is NaN or infinite");
        }
    } else if (t == AV_VT_BIT) {
        unsigned pad = (unsigned) (dim % 8);
        if (pad != 0 && (buf[len - 1] & (0xffu >> pad)) != 0)
            return vb_fail(e, "VB_BIT_PADDING", "padding bits in the last byte are not zero");
    } else {
        const uint8_t *idx = buf + 16;
        const uint8_t *val = idx + 4 * nnz;
        for (int64_t i = 0; i < nnz; i++) {
            if (((rd_u32(val + 4 * i) >> 23) & 0xffu) == 0xffu)
                return vb_fail(e, "VB_NON_FINITE", "a value is NaN or infinite");
        }
        for (int64_t i = 1; i < nnz; i++) {
            if (rd_i32(idx + 4 * i) <= rd_i32(idx + 4 * (i - 1)))
                return vb_fail(e, "VB_SPARSE_UNSORTED", "indices are not strictly ascending");
        }
        for (int64_t i = 0; i < nnz; i++) {
            int32_t ix = rd_i32(idx + 4 * i);
            if (ix < 0 || ix >= dim)
                return vb_fail(e, "VB_SPARSE_INDEX_OUT_OF_RANGE", "an index is outside 0..dim-1");
        }
        for (int64_t i = 0; i < nnz; i++) {
            if ((rd_u32(val + 4 * i) << 1) == 0)
                return vb_fail(e, "VB_SPARSE_ZERO_VALUE", "an explicit zero value");
        }
    }

    if (info != NULL) {
        info->type = t;
        info->flags = flags;
        info->dim = (int32_t) dim;
        info->nnz = (int32_t) nnz;
    }
    return AV_OK;
}

av_status av_vb_decode(const uint8_t *buf, size_t len, av_arena *a, void **pgvec, av_vb_info *info, av_err *e)
{
    av_vb_info local;
    if (av_vb_validate(buf, len, &local, e) != AV_OK)
        return AV_ERR;
    /* validate guarantees 8 <= len <= 16 + 8 * 16000, so these sizes cannot overflow */
    uint8_t *p = av_arena_alloc(a, len, 8);
    if (p == NULL) {
        av_err_set(e, "internal", "vectorbytes decode: out of memory");
        return AV_ERR;
    }
    memcpy(p + VB_HDR, buf + VB_HDR, len - VB_HDR);
    SET_VARSIZE(p, len);
    *pgvec = p;
    if (info != NULL)
        *info = local;
    return AV_OK;
}

av_status av_vb_encode(const void *pgvec, av_vtype t, bool normalized, av_arena *a, uint8_t **out,
                       size_t *len, av_err *e)
{
    if (pgvec == NULL || (unsigned) t < 1 || (unsigned) t > 4) {
        av_err_set(e, "internal", "vectorbytes encode: bad argument");
        return AV_ERR;
    }
    size_t n = (size_t) VARSIZE(pgvec);
    if (n < 8) {
        return vb_fail(e, "VB_TOO_SHORT", "pgvector struct shorter than its fixed header");
    }
    uint8_t *p = av_arena_alloc(a, n, 1);
    if (p == NULL) {
        av_err_set(e, "internal", "vectorbytes encode: out of memory");
        return AV_ERR;
    }
    p[0] = 'A';
    p[1] = 'V';
    p[2] = (uint8_t) (0x10u | (unsigned) t);
    p[3] = normalized ? (uint8_t) AV_VB_FLAG_NORMALIZED : 0;
    memcpy(p + VB_HDR, (const uint8_t *) pgvec + VB_HDR, n - VB_HDR);
    if (av_vb_validate(p, n, NULL, e) != AV_OK)
        return AV_ERR;
    *out = p;
    *len = n;
    return AV_OK;
}
