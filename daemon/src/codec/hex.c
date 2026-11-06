/*
 * codec/hex.c: strict hexadecimal codec (see hex.h).
 */
#include "codec/hex.h"

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

av_status av_hex_encode(const uint8_t *buf, size_t len, av_arena *a, char **out, size_t *outlen, av_err *e)
{
    static const char digits[] = "0123456789abcdef";
    if (len > (SIZE_MAX - 1) / 2) {
        av_err_set(e, "invalid_argument", "hex encode: input too large");
        return AV_ERR;
    }
    char *p = av_arena_alloc(a, len * 2 + 1, 1);
    if (p == NULL) {
        av_err_set(e, "internal", "hex encode: out of memory");
        return AV_ERR;
    }
    for (size_t i = 0; i < len; i++) {
        p[2 * i] = digits[buf[i] >> 4];
        p[2 * i + 1] = digits[buf[i] & 0x0f];
    }
    p[len * 2] = '\0';
    *out = p;
    *outlen = len * 2;
    return AV_OK;
}

av_status av_hex_decode(const char *s, size_t n, av_arena *a, uint8_t **out, size_t *outlen, av_err *e)
{
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        n -= 2;
    }
    if (n % 2 != 0) {
        av_err_set(e, "invalid_argument", "hex decode: odd number of digits");
        return AV_ERR;
    }
    uint8_t *p = av_arena_alloc(a, n / 2, 1);
    if (p == NULL) {
        av_err_set(e, "internal", "hex decode: out of memory");
        return AV_ERR;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_val(s[2 * i]);
        int lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            av_err_set(e, "invalid_argument", "hex decode: invalid character near offset %zu", 2 * i);
            return AV_ERR;
        }
        p[i] = (uint8_t) ((hi << 4) | lo);
    }
    *out = p;
    *outlen = n / 2;
    return AV_OK;
}
