/*
 * codec/vtype.c: av_vtype names. See vtype.h.
 */
#include "codec/vtype.h"

#include <string.h>

#include "postgres.h"

#include "halfvec.h"
#include "sparsevec.h"
#include "utils/varbit.h"
#include "vector.h"

const char *av_vtype_name(av_vtype t)
{
    switch (t) {
    case AV_VT_VECTOR:
        return "vector";
    case AV_VT_HALFVEC:
        return "halfvec";
    case AV_VT_BIT:
        return "bit";
    case AV_VT_SPARSEVEC:
        return "sparsevec";
    }
    return "?";
}

bool av_vtype_parse(const char *s, av_vtype *out)
{
    static const av_vtype all[] = {AV_VT_VECTOR, AV_VT_HALFVEC, AV_VT_BIT, AV_VT_SPARSEVEC};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcmp(s, av_vtype_name(all[i])) == 0) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

int av_pgvec_dim(av_vtype t, const void *v)
{
    switch (t) {
    case AV_VT_VECTOR:
        return ((const Vector *) v)->dim;
    case AV_VT_HALFVEC:
        return ((const HalfVector *) v)->dim;
    case AV_VT_BIT:
        return ((const VarBit *) v)->bit_len;
    case AV_VT_SPARSEVEC:
        return ((const SparseVector *) v)->dim;
    }
    return -1;
}
