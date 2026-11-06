/*
 * codec/vtype.c: vector type names (see vtype.h).
 */
#include "codec/vtype.h"

#include <stddef.h>
#include <string.h>

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
    if (s == NULL)
        return false;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcmp(s, av_vtype_name(all[i])) == 0) {
            *out = all[i];
            return true;
        }
    }
    return false;
}
