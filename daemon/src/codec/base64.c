/*
 * codec/base64.c: strict RFC 4648 base64 (see base64.h).
 */
#include "codec/base64.h"

#include <stdbool.h>

static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

av_status av_base64_encode(const uint8_t *buf, size_t len, av_arena *a, char **out, size_t *outlen, av_err *e)
{
    if (len > (SIZE_MAX - 8) / 4 * 3) {
        av_err_set(e, "invalid_argument", "base64 encode: input too large");
        return AV_ERR;
    }
    size_t n = (len / 3 + (len % 3 != 0 ? 1 : 0)) * 4;
    char *p = av_arena_alloc(a, n + 1, 1);
    if (p == NULL) {
        av_err_set(e, "internal", "base64 encode: out of memory");
        return AV_ERR;
    }
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t) buf[i] << 16) | ((uint32_t) buf[i + 1] << 8) | buf[i + 2];
        p[o++] = b64_alphabet[(v >> 18) & 63];
        p[o++] = b64_alphabet[(v >> 12) & 63];
        p[o++] = b64_alphabet[(v >> 6) & 63];
        p[o++] = b64_alphabet[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = (uint32_t) buf[i] << 16;
        p[o++] = b64_alphabet[(v >> 18) & 63];
        p[o++] = b64_alphabet[(v >> 12) & 63];
        p[o++] = '=';
        p[o++] = '=';
    } else if (len - i == 2) {
        uint32_t v = ((uint32_t) buf[i] << 16) | ((uint32_t) buf[i + 1] << 8);
        p[o++] = b64_alphabet[(v >> 18) & 63];
        p[o++] = b64_alphabet[(v >> 12) & 63];
        p[o++] = b64_alphabet[(v >> 6) & 63];
        p[o++] = '=';
    }
    p[o] = '\0';
    *out = p;
    *outlen = o;
    return AV_OK;
}

av_status av_base64_decode(const char *s, size_t n, av_arena *a, uint8_t **out, size_t *outlen, av_err *e)
{
    if (n % 4 != 0) {
        av_err_set(e, "invalid_argument", "base64 decode: length is not a multiple of 4");
        return AV_ERR;
    }
    size_t pad = 0;
    if (n > 0 && s[n - 1] == '=') {
        pad = (s[n - 2] == '=') ? 2 : 1;
    }
    uint8_t *p = av_arena_alloc(a, n / 4 * 3 - pad, 1);
    if (p == NULL) {
        av_err_set(e, "internal", "base64 decode: out of memory");
        return AV_ERR;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        size_t real = (i + 4 == n) ? 4 - pad : 4;
        uint32_t v = 0;
        for (size_t k = 0; k < real; k++) {
            int d = b64_val(s[i + k]);
            if (d < 0) {
                av_err_set(e, "invalid_argument", "base64 decode: invalid character at offset %zu", i + k);
                return AV_ERR;
            }
            v = (v << 6) | (uint32_t) d;
        }
        if (real == 4) {
            p[o++] = (uint8_t) (v >> 16);
            p[o++] = (uint8_t) (v >> 8);
            p[o++] = (uint8_t) v;
        } else if (real == 3) {
            if ((v & 0x3u) != 0) {
                av_err_set(e, "invalid_argument", "base64 decode: non-zero trailing bits");
                return AV_ERR;
            }
            p[o++] = (uint8_t) (v >> 10);
            p[o++] = (uint8_t) (v >> 2);
        } else {
            if ((v & 0xfu) != 0) {
                av_err_set(e, "invalid_argument", "base64 decode: non-zero trailing bits");
                return AV_ERR;
            }
            p[o++] = (uint8_t) (v >> 4);
        }
    }
    *out = p;
    *outlen = o;
    return AV_OK;
}
