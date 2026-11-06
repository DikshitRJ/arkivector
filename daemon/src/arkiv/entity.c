/*
 * arkiv/entity.c: entity helpers and the JSON entity parser. See entity.h.
 */
#include "arkiv/entity.h"

#include <string.h>

#include "yyjson.h"

/* ------------------------------------------------------------------ hex helpers */

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool av_hex_to_buf(const char *s, size_t slen, uint8_t *out, size_t cap, size_t *outlen)
{
    if (s == NULL || slen < 2 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return false;
    slen -= 2;
    s += 2;
    if ((slen & 1U) != 0 || slen / 2 > cap)
        return false;
    for (size_t i = 0; i < slen / 2; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    if (outlen != NULL)
        *outlen = slen / 2;
    return true;
}

bool av_qty_parse(const char *s, uint64_t *out)
{
    if (s == NULL || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || s[2] == '\0')
        return false;
    uint64_t v = 0;
    for (const char *p = s + 2; *p != '\0'; p++) {
        int d = hexval(*p);
        if (d < 0 || v > (UINT64_MAX >> 4))
            return false;
        v = (v << 4) | (uint64_t) d;
    }
    *out = v;
    return true;
}

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < n; i++) {
        out[2 + 2 * i] = digits[b[i] >> 4];
        out[3 + 2 * i] = digits[b[i] & 0x0F];
    }
    out[2 + 2 * n] = '\0';
}

void av_key_to_hex(const uint8_t key[32], char out[67])
{
    to_hex(key, 32, out);
}

void av_addr_to_hex(const uint8_t addr[20], char out[43])
{
    to_hex(addr, 20, out);
}

bool av_key_from_hex(const char *s, uint8_t key[32])
{
    size_t n = 0;
    return s != NULL && strlen(s) == 66 && av_hex_to_buf(s, 66, key, 32, &n) && n == 32;
}

bool av_addr_from_hex(const char *s, uint8_t addr[20])
{
    size_t n = 0;
    return s != NULL && strlen(s) == 42 && av_hex_to_buf(s, 42, addr, 20, &n) && n == 20;
}

/* ------------------------------------------------------------------ attr / copy */

const av_attr *av_entity_attr(const av_entity *e, const char *name)
{
    if (e == NULL || name == NULL)
        return NULL;
    for (size_t i = 0; i < e->n_attrs; i++)
        if (strcmp(e->attrs[i].name, name) == 0)
            return &e->attrs[i];
    return NULL;
}

av_status av_entity_copy(const av_entity *src, av_arena *a, av_entity *dst, av_err *err)
{
    *dst = *src;
    dst->attrs = NULL;
    if (src->n_attrs == 0)
        return AV_OK;
    dst->attrs = av_arena_alloc(a, src->n_attrs * sizeof(av_attr), 0);
    if (dst->attrs == NULL) {
        av_err_set(err, "internal", "out of memory copying entity");
        return AV_ERR;
    }
    for (size_t i = 0; i < src->n_attrs; i++) {
        dst->attrs[i] = src->attrs[i];
        dst->attrs[i].val = NULL;
        if (src->attrs[i].len > 0) {
            uint8_t *v = av_arena_alloc(a, src->attrs[i].len, 1);
            if (v == NULL) {
                av_err_set(err, "internal", "out of memory copying entity");
                return AV_ERR;
            }
            memcpy(v, src->attrs[i].val, src->attrs[i].len);
            dst->attrs[i].val = v;
        }
    }
    return AV_OK;
}

/* ------------------------------------------------------------------ JSON parser */

static av_status bad(av_err *e, const char *what, const char *detail)
{
    av_err_set(e, "arkiv_rpc_error", "malformed Arkiv response: %s (%s)", what, detail);
    return AV_ERR;
}

static av_status parse_hex_field(yyjson_val *obj, const char *name, uint8_t *out, size_t nbytes,
                                 bool required, av_err *e)
{
    yyjson_val *v = yyjson_obj_get(obj, name);
    if (v == NULL || yyjson_is_null(v))
        return required ? bad(e, "entity field missing", name) : AV_OK;
    const char *s = yyjson_get_str(v);
    size_t n = 0;
    if (s == NULL || yyjson_get_len(v) != 2 + 2 * nbytes ||
        !av_hex_to_buf(s, yyjson_get_len(v), out, nbytes, &n) || n != nbytes)
        return bad(e, "entity hex field", name);
    return AV_OK;
}

static av_status parse_qty_field(yyjson_val *obj, const char *name, uint64_t *out, av_err *e)
{
    yyjson_val *v = yyjson_obj_get(obj, name);
    if (v == NULL || yyjson_is_null(v))
        return AV_OK;
    const char *s = yyjson_get_str(v);
    if (s == NULL || strlen(s) != yyjson_get_len(v) || !av_qty_parse(s, out))
        return bad(e, "entity quantity field", name);
    return AV_OK;
}

/* 32-byte big-endian magnitude := magnitude * 10 + d. false on overflow. */
static bool mag_mul10_add(uint8_t m[32], unsigned d)
{
    unsigned carry = d;
    for (int i = 31; i >= 0; i--) {
        unsigned t = (unsigned) m[i] * 10U + carry;
        m[i] = (uint8_t) (t & 0xFFU);
        carry = t >> 8;
    }
    return carry == 0;
}

/* Decimal string -> signed int256 scaled by 1e18, two's complement. */
static bool dec_to_word(const char *s, size_t len, uint8_t out[32])
{
    size_t i = 0;
    bool neg = false;
    size_t int_digits = 0;
    size_t frac_digits = 0;
    uint8_t m[32] = {0};

    if (len > 0 && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        i = 1;
    }
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, int_digits++)
        if (!mag_mul10_add(m, (unsigned) (s[i] - '0')))
            return false;
    if (i < len && s[i] == '.') {
        i++;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, frac_digits++)
            if (frac_digits >= 18 || !mag_mul10_add(m, (unsigned) (s[i] - '0')))
                return false;
    }
    if (i != len || (int_digits == 0 && frac_digits == 0))
        return false;
    for (size_t k = frac_digits; k < 18; k++)
        if (!mag_mul10_add(m, 0))
            return false;
    /* magnitude must fit int256: <= 2^255 - 1, or exactly 2^255 for negatives */
    if ((m[0] & 0x80U) != 0) {
        bool only_min = m[0] == 0x80;
        for (int k = 1; k < 32 && only_min; k++)
            only_min = m[k] == 0;
        if (!neg || !only_min)
            return false;
    }
    if (neg) { /* two's complement negate */
        unsigned carry = 1;
        for (int k = 31; k >= 0; k--) {
            unsigned t = (unsigned) (uint8_t) ~m[k] + carry;
            m[k] = (uint8_t) (t & 0xFFU);
            carry = t >> 8;
        }
    }
    memcpy(out, m, 32);
    return true;
}

/* Hex quantity -> right-aligned 32-byte word with at most max_digits significant hex digits. */
static bool qty_to_word(const char *s, size_t len, size_t max_digits, uint8_t out[32])
{
    if (len < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || len - 2 > 64)
        return false;
    size_t nd = len - 2;
    size_t first_sig = 0;
    while (first_sig + 1 < nd && s[2 + first_sig] == '0')
        first_sig++;
    if (nd - first_sig > max_digits)
        return false;
    memset(out, 0, 32);
    for (size_t k = 0; k < nd; k++) {
        int d = hexval(s[2 + nd - 1 - k]);
        if (d < 0)
            return false;
        unsigned shift = (k % 2 == 0) ? 0U : 4U;
        out[31 - k / 2] = (uint8_t) (out[31 - k / 2] | ((unsigned) d << shift));
    }
    return true;
}

static uint8_t type_code(const char *t, size_t n)
{
    static const struct {
        const char *name;
        uint8_t code;
    } tab[] = {{"bool", AV_AT_BOOL}, {"i32", AV_AT_I32},   {"u64", AV_AT_U64},
               {"u256", AV_AT_U256}, {"dec", AV_AT_DEC},   {"bytes", AV_AT_BYTES},
               {"str", AV_AT_STR},   {"addr", AV_AT_ADDR}, {"key", AV_AT_KEY}};
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i].name) == n && memcmp(tab[i].name, t, n) == 0)
            return tab[i].code;
    return AV_AT_TOMBSTONE;
}

/* Convert a fixed-width (32-byte word) value. */
static bool word_value(uint8_t type, yyjson_val *vv, uint8_t *buf)
{
    const char *s = yyjson_get_str(vv);
    size_t slen = s != NULL ? yyjson_get_len(vv) : 0;
    size_t n = 0;

    memset(buf, 0, 32);
    switch (type) {
    case AV_AT_BOOL:
        if (!yyjson_is_bool(vv))
            return false;
        buf[31] = yyjson_get_bool(vv) ? 1 : 0;
        return true;
    case AV_AT_I32: {
        if (!yyjson_is_int(vv))
            return false;
        if (yyjson_is_uint(vv) && yyjson_get_uint(vv) > (uint64_t) INT32_MAX)
            return false;
        int64_t x = yyjson_get_sint(vv);
        if (x < INT32_MIN || x > INT32_MAX)
            return false;
        if (x < 0)
            memset(buf, 0xFF, 32);
        for (unsigned k = 0; k < 4; k++)
            buf[31 - k] = (uint8_t) ((uint64_t) x >> (8 * k));
        return true;
    }
    case AV_AT_U64:
    case AV_AT_U256:
        if (s != NULL)
            return strlen(s) == slen && qty_to_word(s, slen, type == AV_AT_U64 ? 16 : 64, buf);
        if (yyjson_is_uint(vv) || (yyjson_is_sint(vv) && yyjson_get_sint(vv) >= 0)) {
            uint64_t x = yyjson_get_uint(vv);
            for (unsigned k = 0; k < 8; k++)
                buf[31 - k] = (uint8_t) (x >> (8 * k));
            return true;
        }
        return false;
    case AV_AT_DEC:
        return s != NULL && strlen(s) == slen && dec_to_word(s, slen, buf);
    case AV_AT_ADDR:
        return s != NULL && slen == 42 && av_hex_to_buf(s, slen, buf + 12, 20, &n) && n == 20;
    default: /* AV_AT_KEY */
        return s != NULL && slen == 66 && av_hex_to_buf(s, slen, buf, 32, &n) && n == 32;
    }
}

static av_status parse_attr(yyjson_val *ao, av_arena *a, av_attr *out, av_err *e)
{
    if (!yyjson_is_obj(ao))
        return bad(e, "attribute", "not an object");
    yyjson_val *vn = yyjson_obj_get(ao, "name");
    yyjson_val *vt = yyjson_obj_get(ao, "type");
    yyjson_val *vv = yyjson_obj_get(ao, "value");
    const char *name = yyjson_get_str(vn);
    const char *type = yyjson_get_str(vt);
    if (name == NULL || type == NULL || vv == NULL)
        return bad(e, "attribute", "name/type/value required");
    size_t nlen = yyjson_get_len(vn);
    if (nlen == 0 || nlen > AV_ATTR_NAME_MAX || strlen(name) != nlen)
        return bad(e, "attribute name", "length 1..32 without NUL");
    memset(out, 0, sizeof *out);
    memcpy(out->name, name, nlen);
    out->type = type_code(type, yyjson_get_len(vt));
    if (out->type == AV_AT_TOMBSTONE)
        return bad(e, "unknown attribute type", out->name);
    yyjson_val *vi = yyjson_obj_get(ao, "indexed");
    out->indexed = yyjson_is_bool(vi) ? yyjson_get_bool(vi) : out->type != AV_AT_BYTES;

    const char *s = yyjson_get_str(vv);
    size_t slen = s != NULL ? yyjson_get_len(vv) : 0;
    uint8_t *buf = NULL;
    if (out->type == AV_AT_STR) {
        if (s == NULL || slen > AV_ATTR_STR_MAX)
            return bad(e, "str value", out->name);
        buf = av_arena_alloc(a, slen > 0 ? slen : 1, 1);
        if (buf != NULL) {
            memcpy(buf, s, slen);
            out->len = slen;
        }
    } else if (out->type == AV_AT_BYTES) {
        if (s == NULL || slen > 2 + 2 * (size_t) AV_ATTR_BYTES_MAX)
            return bad(e, "bytes value", out->name);
        buf = av_arena_alloc(a, slen / 2 + 1, 1);
        if (buf != NULL && !av_hex_to_buf(s, slen, buf, slen / 2 + 1, &out->len))
            return bad(e, "bytes hex", out->name);
    } else {
        buf = av_arena_alloc(a, 32, 1);
        if (buf != NULL) {
            if (!word_value(out->type, vv, buf))
                return bad(e, "attribute value", out->name);
            out->len = 32;
        }
    }
    if (buf == NULL) {
        av_err_set(e, "internal", "out of memory parsing entity");
        return AV_ERR;
    }
    out->val = buf;
    return AV_OK;
}

av_status av_entity_from_json(yyjson_val *obj, av_arena *a, av_entity *out, av_err *e)
{
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(obj))
        return bad(e, "entity", "expected object");
    if (parse_hex_field(obj, "key", out->key, 32, true, e) != AV_OK ||
        parse_hex_field(obj, "owner", out->owner, 20, false, e) != AV_OK ||
        parse_hex_field(obj, "creator", out->creator, 20, false, e) != AV_OK ||
        parse_qty_field(obj, "createdAt", &out->created_at, e) != AV_OK ||
        parse_qty_field(obj, "updatedAt", &out->updated_at, e) != AV_OK ||
        parse_qty_field(obj, "expiresAt", &out->expires_at, e) != AV_OK)
        return AV_ERR;

    yyjson_val *attrs = yyjson_obj_get(obj, "attributes");
    if (attrs == NULL || yyjson_is_null(attrs))
        return AV_OK;
    if (!yyjson_is_arr(attrs))
        return bad(e, "attributes", "not an array");
    size_t n = yyjson_arr_size(attrs);
    if (n > AV_ENTITY_ATTRS_MAX)
        return bad(e, "attributes", "too many");
    if (n == 0)
        return AV_OK;
    out->attrs = av_arena_alloc(a, n * sizeof(av_attr), 0);
    if (out->attrs == NULL) {
        av_err_set(e, "internal", "out of memory parsing entity");
        return AV_ERR;
    }
    size_t idx, max;
    yyjson_val *ao;
    yyjson_arr_foreach(attrs, idx, max, ao)
    {
        if (parse_attr(ao, a, &out->attrs[idx], e) != AV_OK)
            return AV_ERR;
        out->n_attrs = idx + 1;
    }
    return AV_OK;
}
