/*
 * codec/text.c: see text.h.
 */
#include "codec/text.h"

#include <string.h>

#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "codec/vectorbytes.h"
#include "sparsevec.h"

extern Datum vector_in(PG_FUNCTION_ARGS);
extern Datum halfvec_in(PG_FUNCTION_ARGS);
extern Datum sparsevec_in(PG_FUNCTION_ARGS);
extern Datum bit_in(PG_FUNCTION_ARGS);
extern Datum vector_out(PG_FUNCTION_ARGS);
extern Datum halfvec_out(PG_FUNCTION_ARGS);
extern Datum sparsevec_out(PG_FUNCTION_ARGS);
extern Datum bit_out(PG_FUNCTION_ARGS);

size_t av_pgvec_size(const void *v)
{
    return (size_t) VARSIZE(v);
}

static PGFunction in_fn(av_vtype t)
{
    switch (t) {
    case AV_VT_VECTOR:
        return vector_in;
    case AV_VT_HALFVEC:
        return halfvec_in;
    case AV_VT_SPARSEVEC:
        return sparsevec_in;
    case AV_VT_BIT:
        return bit_in;
    }
    return NULL;
}

static PGFunction out_fn(av_vtype t)
{
    switch (t) {
    case AV_VT_VECTOR:
        return vector_out;
    case AV_VT_HALFVEC:
        return halfvec_out;
    case AV_VT_SPARSEVEC:
        return sparsevec_out;
    case AV_VT_BIT:
        return bit_out;
    }
    return NULL;
}

/* Per-type upper limits shared with VectorBytes (dims, nnz), applied after parsing. */
static bool within_limits(av_vtype t, const void *v)
{
    int64_t nnz = t == AV_VT_SPARSEVEC ? ((const SparseVector *) v)->nnz : 0;
    if (av_pgvec_dim(t, v) == 0) {
        return true; /* pgvector accepts '' for bit; upper limits only here */
    }
    size_t size;
    return av_vb_encoded_size(t, av_pgvec_dim(t, v), nnz, &size);
}

av_status av_text_to_pgvecn(av_vtype t, const char *s, size_t len, int32_t typmod_dim, av_arena *a,
                            void **out, av_err *e)
{
    PGFunction fn = in_fn(t);
    if (fn == NULL || s == NULL) {
        av_err_set(e, "invalid_vector", "unknown vector type");
        return AV_ERR;
    }
    if (len > AV_TEXT_MAX_LEN) {
        av_err_set(e, "invalid_vector", "vector literal longer than %u bytes", (unsigned) AV_TEXT_MAX_LEN);
        return AV_ERR;
    }
    if (memchr(s, '\0', len) != NULL) {
        av_err_set(e, "invalid_vector", "vector literal contains a NUL byte");
        return AV_ERR;
    }
    MemoryContext old = CurrentMemoryContext;
    MemoryContext volatile ctx = NULL;
    volatile av_status st = AV_OK;
    PG_TRY();
    {
        ctx = AvContextCreate(NULL, "text_in", 8192, AV_TEXT_CTX_CAP);
        MemoryContextSwitchTo(ctx);
        char *cstr = pnstrdup(s, len);
        Datum d = DirectFunctionCall3(fn, CStringGetDatum(cstr), ObjectIdGetDatum(InvalidOid),
                                      Int32GetDatum(typmod_dim > 0 ? typmod_dim : -1));
        const void *v = DatumGetPointer(d);
        if (!within_limits(t, v)) {
            av_err_set(e, "invalid_vector", "%s exceeds the dimension limits of its type", av_vtype_name(t));
            st = AV_ERR;
        } else {
            size_t n = (size_t) VARSIZE(v);
            void *dst = av_arena_alloc(a, n, 8);
            if (dst == NULL) {
                av_err_set(e, "internal", "out of memory");
                st = AV_ERR;
            } else {
                memcpy(dst, v, n);
                *out = dst;
            }
        }
    }
    PG_CATCH();
    {
        av_err_set(e, "invalid_vector", "%s", av_pg_error_data()->message);
        FlushErrorState();
        st = AV_ERR;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    if (ctx != NULL) {
        MemoryContextDelete(ctx);
    }
    return st;
}

av_status av_text_to_pgvec(av_vtype t, const char *s, int32_t typmod_dim, av_arena *a, void **out, av_err *e)
{
    if (s == NULL) {
        av_err_set(e, "invalid_vector", "missing vector literal");
        return AV_ERR;
    }
    return av_text_to_pgvecn(t, s, strnlen(s, (size_t) AV_TEXT_MAX_LEN + 1), typmod_dim, a, out, e);
}

av_status av_pgvec_to_text(av_vtype t, const void *v, av_arena *a, char **out, av_err *e)
{
    PGFunction fn = out_fn(t);
    if (fn == NULL || v == NULL) {
        av_err_set(e, "invalid_vector", "unknown vector type");
        return AV_ERR;
    }
    MemoryContext old = CurrentMemoryContext;
    MemoryContext volatile ctx = NULL;
    volatile av_status st = AV_OK;
    PG_TRY();
    {
        ctx = AvContextCreate(NULL, "text_out", 8192, AV_TEXT_CTX_CAP);
        MemoryContextSwitchTo(ctx);
        char *txt = DatumGetCString(DirectFunctionCall1(fn, PointerGetDatum(v)));
        char *dst = av_arena_strdup(a, txt);
        if (dst == NULL) {
            av_err_set(e, "internal", "out of memory");
            st = AV_ERR;
        } else {
            *out = dst;
        }
    }
    PG_CATCH();
    {
        av_err_set(e, "invalid_vector", "%s", av_pg_error_data()->message);
        FlushErrorState();
        st = AV_ERR;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    if (ctx != NULL) {
        MemoryContextDelete(ctx);
    }
    return st;
}
