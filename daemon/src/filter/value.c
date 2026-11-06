/*
 * filter/value.c: canonical typed values (see value.h).
 *
 * 256-bit arithmetic works on 32-byte big-endian arrays; only multiply-by-10-and-add,
 * divide-by-10 and negate are needed. The EIP-55 checksum uses a private Keccak-256
 * (single-block) so this module has no dependency on arkiv/keccak.c.
 */
#include "filter/value.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* type names                                                                */
/* ------------------------------------------------------------------------- */

static const struct {
    const char *name;
    av_attr_type type;
} k_types[] = {{"bool", AV_AT_BOOL}, {"i32", AV_AT_I32},     {"u64", AV_AT_U64}, {"u256", AV_AT_U256},
               {"dec", AV_AT_DEC},   {"str", AV_AT_STR},     {"addr", AV_AT_ADDR}, {"key", AV_AT_KEY},
               {"bytes", AV_AT_BYTES}};

const char *av_type_name(av_attr_type t)
{
    for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        if (k_types[i].type == t) {
            return k_types[i].name;
        }
    }
    return t == AV_AT_TOMBSTONE ? "tombstone" : "unknown";
}

bool av_type_from_name(const char *s, size_t n, av_attr_type *out)
{
    for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        if (strlen(k_types[i].name) == n && memcmp(k_types[i].name, s, n) == 0) {
            *out = k_types[i].type;
            return true;
        }
    }
    return false;
}

bool av_type_filterable(av_attr_type t)
{
    return t == AV_AT_BOOL || t == AV_AT_I32 || t == AV_AT_U64 || t == AV_AT_U256 || t == AV_AT_DEC ||
           t == AV_AT_STR || t == AV_AT_ADDR || t == AV_AT_KEY;
}

/* ------------------------------------------------------------------------- */
/* 256-bit helpers                                                           */
/* ------------------------------------------------------------------------- */

/* x = x * 10 + d. Returns true on overflow (x is then unspecified). */
static bool u256_mul10_add(uint8_t x[32], unsigned d)
{
    unsigned carry = d;
    for (int i = 31; i >= 0; i--) {
        unsigned t = (unsigned) x[i] * 10u + carry;
        x[i] = (uint8_t) (t & 0xffu);
        carry = t >> 8;
    }
    return carry != 0;
}

/* x /= 10, returns the remainder. */
static unsigned u256_divmod10(uint8_t x[32])
{
    unsigned rem = 0;
    for (int i = 0; i < 32; i++) {
        unsigned t = rem * 256u + x[i];
        x[i] = (uint8_t) (t / 10u);
        rem = t % 10u;
    }
    return rem;
}

static bool u256_is_zero(const uint8_t x[32])
{
    for (int i = 0; i < 32; i++) {
        if (x[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Two's complement negate in place. */
static void u256_negate(uint8_t x[32])
{
    unsigned carry = 1;
    for (int i = 31; i >= 0; i--) {
        unsigned t = (unsigned) (uint8_t) ~x[i] + carry;
        x[i] = (uint8_t) (t & 0xffu);
        carry = t >> 8;
    }
}

static void word_from_u64(uint8_t w[32], uint64_t x)
{
    memset(w, 0, 32);
    for (unsigned i = 0; i < 8; i++) {
        w[31 - i] = (uint8_t) (x >> (8 * i));
    }
}

/* ------------------------------------------------------------------------- */
/* UTF-8                                                                     */
/* ------------------------------------------------------------------------- */

bool av_utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            if (c == 0xE0 && s[i + 1] < 0xA0) {
                return false; /* overlong */
            }
            if (c == 0xED && s[i + 1] > 0x9F) {
                return false; /* surrogate */
            }
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 ||
                (s[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            if (c == 0xF0 && s[i + 1] < 0x90) {
                return false; /* overlong */
            }
            if (c == 0xF4 && s[i + 1] > 0x8F) {
                return false; /* > U+10FFFF */
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* builders                                                                  */
/* ------------------------------------------------------------------------- */

static void value_init(av_value *v, av_attr_type t, size_t len)
{
    memset(v, 0, sizeof(*v));
    v->type = t;
    v->len = len;
}

void av_value_bool(av_value *v, bool x)
{
    value_init(v, AV_AT_BOOL, 32);
    v->b[31] = x ? 1 : 0;
}

void av_value_i32(av_value *v, int32_t x)
{
    value_init(v, AV_AT_I32, 32);
    memset(v->b, x < 0 ? 0xff : 0x00, 32);
    uint32_t u = (uint32_t) x;
    for (unsigned i = 0; i < 4; i++) {
        v->b[31 - i] = (uint8_t) (u >> (8 * i));
    }
}

void av_value_u64(av_value *v, uint64_t x)
{
    value_init(v, AV_AT_U64, 32);
    word_from_u64(v->b, x);
}

void av_value_addr(av_value *v, const uint8_t addr[20])
{
    value_init(v, AV_AT_ADDR, 32);
    memcpy(v->b + 12, addr, 20);
}

void av_value_key(av_value *v, const uint8_t key[32])
{
    value_init(v, AV_AT_KEY, 32);
    memcpy(v->b, key, 32);
}

void av_value_u256_word(av_value *v, const uint8_t word[32])
{
    value_init(v, AV_AT_U256, 32);
    memcpy(v->b, word, 32);
}

void av_value_dec_word(av_value *v, const uint8_t word[32])
{
    value_init(v, AV_AT_DEC, 32);
    memcpy(v->b, word, 32);
}

bool av_value_str(av_value *v, const uint8_t *s, size_t n)
{
    if (n > AV_ATTR_STR_MAX || (n > 0 && s == NULL) || !av_utf8_valid(s, n)) {
        return false;
    }
    value_init(v, AV_AT_STR, n);
    if (n > 0) {
        memcpy(v->b, s, n);
    }
    return true;
}

static bool all_byte(const uint8_t *p, size_t n, uint8_t x)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != x) {
            return false;
        }
    }
    return true;
}

bool av_value_from_stored(av_attr_type t, const uint8_t *p, size_t n, av_value *out)
{
    if (!av_type_filterable(t) || (n > 0 && p == NULL)) {
        return false;
    }
    if (t == AV_AT_STR) {
        return av_value_str(out, p, n);
    }
    if (n != 32) {
        return false;
    }
    switch (t) {
    case AV_AT_BOOL:
        if (!all_byte(p, 31, 0) || p[31] > 1) {
            return false;
        }
        break;
    case AV_AT_I32:
        if (!all_byte(p, 28, (p[28] & 0x80) != 0 ? 0xff : 0x00)) {
            return false;
        }
        break;
    case AV_AT_U64:
        if (!all_byte(p, 24, 0)) {
            return false;
        }
        break;
    case AV_AT_ADDR:
        if (!all_byte(p, 12, 0)) {
            return false;
        }
        break;
    default:
        break; /* u256, dec, key: any 32 bytes */
    }
    value_init(out, t, 32);
    memcpy(out->b, p, 32);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Keccak-256 (one block, input <= 135 bytes) for EIP-55                     */
/* ------------------------------------------------------------------------- */

static const uint64_t k_rc[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
static const unsigned k_rot[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
static const unsigned k_pi[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

static uint64_t rotl64(uint64_t x, unsigned n)
{
    return (x << n) | (x >> (64u - n));
}

static void keccakf(uint64_t st[25])
{
    for (size_t r = 0; r < 24; r++) {
        uint64_t bc[5];
        for (size_t i = 0; i < 5; i++) {
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        }
        for (size_t i = 0; i < 5; i++) {
            uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (size_t j = 0; j < 25; j += 5) {
                st[j + i] ^= t;
            }
        }
        uint64_t t = st[1];
        for (size_t i = 0; i < 24; i++) {
            unsigned j = k_pi[i];
            uint64_t b0 = st[j];
            st[j] = rotl64(t, k_rot[i]);
            t = b0;
        }
        for (size_t j = 0; j < 25; j += 5) {
            for (size_t i = 0; i < 5; i++) {
                bc[i] = st[j + i];
            }
            for (size_t i = 0; i < 5; i++) {
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
            }
        }
        st[0] ^= k_rc[r];
    }
}

static void keccak256_small(const uint8_t *in, size_t n, uint8_t out[32])
{
    uint8_t blk[136];
    memset(blk, 0, sizeof(blk));
    memcpy(blk, in, n); /* callers guarantee n <= 135 */
    blk[n] ^= 0x01;
    blk[135] ^= 0x80;
    uint64_t st[25];
    memset(st, 0, sizeof(st));
    for (size_t i = 0; i < 17; i++) {
        uint64_t w = 0;
        for (size_t k = 8; k > 0; k--) {
            w = (w << 8) | blk[i * 8 + k - 1];
        }
        st[i] = w;
    }
    keccakf(st);
    for (size_t i = 0; i < 32; i++) {
        out[i] = (uint8_t) (st[i / 8] >> (8 * (i % 8)));
    }
}

/* ------------------------------------------------------------------------- */
/* parsing                                                                   */
/* ------------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool has_0x(const char *s, size_t n)
{
    return n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

/* Decode exactly nbytes*2 hex digits after 0x into out[0..nbytes). */
static bool hex_fixed(const char *s, size_t n, size_t nbytes, uint8_t *out)
{
    if (!has_0x(s, n) || n != 2 + nbytes * 2) {
        return false;
    }
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hexval(s[2 + 2 * i]);
        int lo = hexval(s[3 + 2 * i]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

static bool addr_checksum_ok(const char *s) /* s = 40 hex chars */
{
    bool any_lower = false;
    bool any_upper = false;
    for (size_t i = 0; i < 40; i++) {
        if (s[i] >= 'a' && s[i] <= 'f') {
            any_lower = true;
        } else if (s[i] >= 'A' && s[i] <= 'F') {
            any_upper = true;
        }
    }
    if (!(any_lower && any_upper)) {
        return true; /* all-lower or all-upper: no checksum claimed */
    }
    uint8_t low[40];
    for (size_t i = 0; i < 40; i++) {
        low[i] = (uint8_t) ((s[i] >= 'A' && s[i] <= 'F') ? s[i] - 'A' + 'a' : s[i]);
    }
    uint8_t h[32];
    keccak256_small(low, 40, h);
    for (size_t i = 0; i < 40; i++) {
        unsigned nib = (unsigned) (h[i / 2] >> ((i % 2 == 0) ? 4 : 0)) & 0xfu;
        if (low[i] >= 'a' && low[i] <= 'f') {
            bool want_upper = nib >= 8;
            bool is_upper = s[i] >= 'A' && s[i] <= 'F';
            if (want_upper != is_upper) {
                return false;
            }
        }
    }
    return true;
}

static bool parse_u256_text(const char *s, size_t n, uint8_t w[32], const char **why)
{
    memset(w, 0, 32);
    if (has_0x(s, n)) {
        size_t nd = n - 2;
        if (nd == 0 || nd > 64) {
            *why = "u256 hex must have 1 to 64 digits";
            return false;
        }
        for (size_t i = 0; i < nd; i++) {
            int h = hexval(s[2 + i]);
            if (h < 0) {
                *why = "invalid hex digit";
                return false;
            }
            size_t pos = nd - 1 - i; /* nibble index from least significant */
            unsigned v = (pos % 2 == 0) ? (unsigned) h : (unsigned) h << 4;
            w[31 - pos / 2] = (uint8_t) (w[31 - pos / 2] | v);
        }
        return true;
    }
    if (n == 0) {
        *why = "empty integer";
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) {
            *why = "invalid integer";
            return false;
        }
        if (u256_mul10_add(w, (unsigned) (s[i] - '0'))) {
            *why = "value out of range for u256";
            return false;
        }
    }
    return true;
}

static bool parse_dec_text(const char *s, size_t n, uint8_t w[32], const char **why)
{
    size_t i = 0;
    bool neg = false;
    if (i < n && s[i] == '-') {
        neg = true;
        i++;
    }
    size_t whole_start = i;
    while (i < n && is_digit(s[i])) {
        i++;
    }
    size_t whole_end = i;
    if (whole_end == whole_start) {
        *why = "invalid decimal";
        return false;
    }
    size_t frac_start = 0;
    size_t frac_end = 0;
    if (i < n && s[i] == '.') {
        i++;
        frac_start = i;
        while (i < n && is_digit(s[i])) {
            i++;
        }
        frac_end = i;
        if (frac_end == frac_start) {
            *why = "invalid decimal";
            return false;
        }
    }
    if (i != n) {
        *why = "invalid decimal";
        return false;
    }
    size_t nfrac = frac_end - frac_start;
    if (nfrac > 18) {
        *why = "dec supports at most 18 fraction digits";
        return false;
    }
    memset(w, 0, 32);
    bool ovf = false;
    for (size_t k = whole_start; k < whole_end && !ovf; k++) {
        ovf = u256_mul10_add(w, (unsigned) (s[k] - '0'));
    }
    for (size_t k = frac_start; k < frac_end && !ovf; k++) {
        ovf = u256_mul10_add(w, (unsigned) (s[k] - '0'));
    }
    for (size_t k = nfrac; k < 18 && !ovf; k++) {
        ovf = u256_mul10_add(w, 0);
    }
    if (!ovf && (w[0] & 0x80) != 0) {
        /* magnitude >= 2^255: only -2^255 is representable */
        uint8_t min[32] = {0x80};
        ovf = !(neg && memcmp(w, min, 32) == 0);
    }
    if (ovf) {
        *why = "dec out of signed 256-bit range";
        return false;
    }
    if (neg) {
        u256_negate(w); /* -0 stays 0 */
    }
    return true;
}

/* Parse digits (optional '-') into a magnitude <= max_mag. */
static bool parse_small_int(const char *s, size_t n, bool allow_neg, uint64_t max_mag, uint64_t *mag, bool *neg,
                            const char **why)
{
    size_t i = 0;
    *neg = false;
    if (allow_neg && n > 0 && s[0] == '-') {
        *neg = true;
        i = 1;
    }
    if (i >= n) {
        *why = "empty integer";
        return false;
    }
    uint64_t x = 0;
    for (; i < n; i++) {
        if (!is_digit(s[i])) {
            *why = "invalid integer";
            return false;
        }
        uint64_t d = (uint64_t) (s[i] - '0');
        if (x > (max_mag - d) / 10) {
            *why = "integer out of range";
            return false;
        }
        x = x * 10 + d;
    }
    *mag = x;
    return true;
}

static bool ieq(const char *s, size_t n, const char *lit)
{
    size_t m = strlen(lit);
    if (n != m) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

bool av_value_parse(av_attr_type t, const char *s, size_t n, av_value *out, const char **why)
{
    const char *dummy;
    if (why == NULL) {
        why = &dummy;
    }
    if (s == NULL && n > 0) {
        *why = "null input";
        return false;
    }
    uint8_t w[32];
    switch (t) {
    case AV_AT_BOOL:
        if (ieq(s, n, "true")) {
            av_value_bool(out, true);
        } else if (ieq(s, n, "false")) {
            av_value_bool(out, false);
        } else {
            *why = "bool must be true or false";
            return false;
        }
        return true;
    case AV_AT_I32: {
        uint64_t mag = 0;
        bool neg = false;
        if (!parse_small_int(s, n, true, 2147483648ULL, &mag, &neg, why)) {
            return false;
        }
        if (!neg && mag > 2147483647ULL) {
            *why = "integer out of range";
            return false;
        }
        av_value_i32(out, neg ? (int32_t) (-(int64_t) mag) : (int32_t) mag);
        return true;
    }
    case AV_AT_U64: {
        uint64_t mag = 0;
        bool neg = false;
        if (!parse_small_int(s, n, false, UINT64_MAX, &mag, &neg, why)) {
            return false;
        }
        av_value_u64(out, mag);
        return true;
    }
    case AV_AT_U256:
        if (!parse_u256_text(s, n, w, why)) {
            return false;
        }
        av_value_u256_word(out, w);
        return true;
    case AV_AT_DEC:
        if (!parse_dec_text(s, n, w, why)) {
            return false;
        }
        av_value_dec_word(out, w);
        return true;
    case AV_AT_STR:
        if (n > AV_ATTR_STR_MAX) {
            *why = "str exceeds 128 bytes";
            return false;
        }
        if (!av_value_str(out, (const uint8_t *) s, n)) {
            *why = "str is not valid UTF-8";
            return false;
        }
        return true;
    case AV_AT_ADDR:
        if (!hex_fixed(s, n, 20, w)) {
            *why = "address must be 0x followed by 40 hex digits";
            return false;
        }
        if (!addr_checksum_ok(s + 2)) {
            *why = "EIP-55 checksum mismatch";
            return false;
        }
        av_value_addr(out, w);
        return true;
    case AV_AT_KEY:
        if (!hex_fixed(s, n, 32, w)) {
            *why = "key must be 0x followed by 64 hex digits";
            return false;
        }
        av_value_key(out, w);
        return true;
    case AV_AT_BYTES:
        *why = "bytes cannot be filtered";
        return false;
    default:
        *why = "unknown type";
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* formatting                                                                */
/* ------------------------------------------------------------------------- */

/* Decimal digits of the unsigned 256-bit magnitude, least significant first. Returns count (>= 1). */
static size_t u256_digits(const uint8_t word[32], char d[80])
{
    uint8_t x[32];
    memcpy(x, word, 32);
    size_t n = 0;
    do {
        d[n++] = (char) ('0' + (int) u256_divmod10(x));
    } while (!u256_is_zero(x));
    return n;
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} sink;

static void sink_put(sink *s, char c)
{
    if (s->cap > 0 && s->len + 1 < s->cap) {
        s->buf[s->len] = c;
    }
    s->len++;
}

static void sink_puts(sink *s, const char *p)
{
    for (; *p != '\0'; p++) {
        sink_put(s, *p);
    }
}

static size_t sink_end(sink *s)
{
    if (s->cap > 0) {
        s->buf[s->len < s->cap ? s->len : s->cap - 1] = '\0';
    }
    return s->len;
}

size_t av_value_format(const av_value *v, char *buf, size_t cap)
{
    sink s = {buf, cap, 0};
    char d[80];
    static const char hexd[] = "0123456789abcdef";
    switch (v->type) {
    case AV_AT_BOOL:
        sink_puts(&s, v->b[31] != 0 ? "true" : "false");
        break;
    case AV_AT_I32: {
        uint32_t u = 0;
        for (size_t i = 28; i < 32; i++) {
            u = (u << 8) | v->b[i];
        }
        char tmp[16];
        (void) snprintf(tmp, sizeof(tmp), "%d", (int) (int32_t) u);
        sink_puts(&s, tmp);
        break;
    }
    case AV_AT_U64:
    case AV_AT_U256: {
        size_t nd = u256_digits(v->b, d);
        while (nd > 0) {
            sink_put(&s, d[--nd]);
        }
        break;
    }
    case AV_AT_DEC: {
        uint8_t mag[32];
        memcpy(mag, v->b, 32);
        bool neg = (mag[0] & 0x80) != 0;
        if (neg) {
            u256_negate(mag);
        }
        size_t nd = u256_digits(mag, d);
        char pad[80];
        size_t total = nd < 19 ? 19 : nd; /* at least one whole digit */
        for (size_t i = 0; i < total; i++) {
            pad[i] = i < nd ? d[i] : '0';
        }
        size_t lo = 0; /* skip trailing fraction zeros */
        while (lo < 18 && pad[lo] == '0') {
            lo++;
        }
        if (neg) {
            sink_put(&s, '-');
        }
        for (size_t i = total; i > 18; i--) {
            sink_put(&s, pad[i - 1]);
        }
        if (lo < 18) {
            sink_put(&s, '.');
            for (size_t i = 18; i > lo; i--) {
                sink_put(&s, pad[i - 1]);
            }
        }
        break;
    }
    case AV_AT_STR:
        for (size_t i = 0; i < v->len; i++) {
            sink_put(&s, (char) v->b[i]);
        }
        break;
    case AV_AT_ADDR:
    case AV_AT_KEY: {
        size_t off = v->type == AV_AT_ADDR ? 12 : 0;
        sink_put(&s, '0');
        sink_put(&s, 'x');
        for (size_t i = off; i < 32; i++) {
            sink_put(&s, hexd[v->b[i] >> 4]);
            sink_put(&s, hexd[v->b[i] & 0xf]);
        }
        break;
    }
    default:
        break;
    }
    return sink_end(&s);
}

/* ------------------------------------------------------------------------- */
/* comparison                                                                */
/* ------------------------------------------------------------------------- */

static int cmp_sign(int c)
{
    return (c > 0) - (c < 0);
}

int av_value_cmp(const av_value *a, const av_value *b)
{
    if (a->type != b->type) {
        return a->type < b->type ? -1 : 1;
    }
    switch (a->type) {
    case AV_AT_I32:
    case AV_AT_DEC: {
        bool na = (a->b[0] & 0x80) != 0;
        bool nb = (b->b[0] & 0x80) != 0;
        if (na != nb) {
            return na ? -1 : 1;
        }
        return cmp_sign(memcmp(a->b, b->b, 32));
    }
    case AV_AT_STR: {
        size_t m = a->len < b->len ? a->len : b->len;
        int c = m > 0 ? memcmp(a->b, b->b, m) : 0;
        if (c != 0) {
            return cmp_sign(c);
        }
        return a->len == b->len ? 0 : (a->len < b->len ? -1 : 1);
    }
    default:
        return cmp_sign(memcmp(a->b, b->b, 32));
    }
}

bool av_value_equal(const av_value *a, const av_value *b)
{
    return av_value_cmp(a, b) == 0;
}
