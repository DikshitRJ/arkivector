#ifndef AV_CODEC_VTYPE_H
#define AV_CODEC_VTYPE_H
#include <stdbool.h>
typedef enum { AV_VT_VECTOR = 1, AV_VT_HALFVEC = 2, AV_VT_BIT = 3, AV_VT_SPARSEVEC = 4 } av_vtype;
const char *av_vtype_name(av_vtype t); /* "vector" | "halfvec" | "bit" | "sparsevec" | "?" */
bool av_vtype_parse(const char *s, av_vtype *out);
#endif
