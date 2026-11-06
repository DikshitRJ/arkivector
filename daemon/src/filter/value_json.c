/*
 * filter/value_json.c: stored attribute -> TypedValue JSON (see value_json.h).
 */
#include "filter/value_json.h"

#include <stdint.h>

#include "codec/base64.h"
#include "filter/value.h"

yyjson_mut_val *av_attr_to_typed_json(yyjson_mut_doc *doc, const av_attr *at, av_arena *arena)
{
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (obj == NULL) {
        return NULL;
    }
    yyjson_mut_obj_add_str(doc, obj, "type", av_type_name((av_attr_type) at->type));

    av_value v;
    switch ((av_attr_type) at->type) {
    case AV_AT_BOOL:
        yyjson_mut_obj_add_bool(doc, obj, "value", at->len >= 32 && at->val[31] != 0);
        break;
    case AV_AT_I32: {
        int64_t x = 0;
        if (av_value_from_stored(AV_AT_I32, at->val, at->len, &v)) {
            /* Canonical word: big-endian two's complement, the value is in the low 4 bytes. */
            uint32_t u = ((uint32_t) v.b[28] << 24) | ((uint32_t) v.b[29] << 16) | ((uint32_t) v.b[30] << 8) |
                         (uint32_t) v.b[31];
            x = u >= 0x80000000U ? (int64_t) u - 0x100000000LL : (int64_t) u;
        }
        yyjson_mut_obj_add_int(doc, obj, "value", x);
        break;
    }
    case AV_AT_STR:
        yyjson_mut_obj_add_strncpy(doc, obj, "value", (const char *) at->val, at->len);
        break;
    case AV_AT_BYTES: {
        char *b64 = NULL;
        size_t b64_len = 0;
        av_err err;
        av_err_clear(&err);
        if (av_base64_encode(at->val, at->len, arena, &b64, &b64_len, &err) == AV_OK) {
            yyjson_mut_obj_add_strncpy(doc, obj, "value", b64, b64_len);
        } else {
            yyjson_mut_obj_add_strcpy(doc, obj, "value", "");
        }
        break;
    }
    default: {
        char buf[AV_VALUE_FORMAT_MAX];
        buf[0] = '\0';
        if (av_value_from_stored((av_attr_type) at->type, at->val, at->len, &v)) {
            av_value_format(&v, buf, sizeof(buf));
        }
        yyjson_mut_obj_add_strcpy(doc, obj, "value", buf);
        break;
    }
    }
    return obj;
}
