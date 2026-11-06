/*
 * codec/text.c: see text.h.
 */
#include "codec/text.h"

#include <string.h>

#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"
#include "varatt.h"

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

av_status av_text_to_pgvec(av_vtype t, const char *s, int32_t typmod_dim, av_arena *a, void **out, av_err *e)
{
    PGFunction fn = in_fn(t);
    if (fn == NULL || s == NULL) {
        av_err_set(e, "invalid_vector", "unknown vector type");
        return AV_ERR;
    }
    MemoryContext ctx = AvContextCreate(NULL, "text_in", 8192, 64u * 1024 * 1024);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    volatile av_status st = AV_OK;
    PG_TRY();
    {
        Datum d = DirectFunctionCall3(fn, CStringGetDatum(s), ObjectIdGetDatum(InvalidOid),
                                      Int32GetDatum(typmod_dim > 0 ? typmod_dim : -1));
        const void *v = DatumGetPointer(d);
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
    PG_CATCH();
    {
        av_err_set(e, "invalid_vector", "%s", av_pg_error_data()->message);
        FlushErrorState();
        st = AV_ERR;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    return st;
}

av_status av_pgvec_to_text(av_vtype t, const void *v, av_arena *a, char **out, av_err *e)
{
    PGFunction fn = out_fn(t);
    if (fn == NULL || v == NULL) {
        av_err_set(e, "invalid_vector", "unknown vector type");
        return AV_ERR;
    }
    MemoryContext ctx = AvContextCreate(NULL, "text_out", 8192, 0);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    volatile av_status st = AV_OK;
    PG_TRY();
    {
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
    MemoryContextDelete(ctx);
    return st;
}
