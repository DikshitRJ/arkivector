/*
 * arkiv/query_builder.c: see query_builder.h.
 */
#include "arkiv/query_builder.h"

#include <stdio.h>
#include <string.h>

#include "arkiv/entity.h"

static av_status too_long(av_err *e)
{
    av_err_set(e, "invalid_argument", "query exceeds the maximum length");
    return AV_ERR;
}

static av_status invalid(av_err *e, const char *what)
{
    av_err_set(e, "invalid_argument", "invalid query value: %s", what);
    return AV_ERR;
}

void av_qb_init(av_qb *q, char *buf, size_t cap)
{
    q->buf = buf;
    q->cap = cap;
    q->len = 0;
    if (cap > 0)
        buf[0] = '\0';
}

av_status av_qb_raw(av_qb *q, const char *s, av_err *e)
{
    size_t n = strlen(s);
    if (q->cap == 0 || n > q->cap - 1 - q->len)
        return too_long(e);
    memcpy(q->buf + q->len, s, n + 1);
    q->len += n;
    return AV_OK;
}

av_status av_qb_and(av_qb *q, av_err *e)
{
    return q->len == 0 ? AV_OK : av_qb_raw(q, " AND ", e);
}

av_status av_qb_or(av_qb *q, av_err *e)
{
    return q->len == 0 ? AV_OK : av_qb_raw(q, " OR ", e);
}

av_status av_qb_lit_str(const char *s, size_t len, char *out, size_t cap, av_err *e)
{
    if (len > AV_QB_STR_MAX)
        return invalid(e, "str literal longer than 128 bytes");
    if (cap < 2 * len + 7)
        return too_long(e);
    size_t w = 0;
    memcpy(out, "str('", 5);
    w = 5;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\0')
            return invalid(e, "NUL byte in str literal");
        if (s[i] == '\'')
            out[w++] = '\'';
        out[w++] = s[i];
    }
    out[w++] = '\'';
    out[w++] = ')';
    out[w] = '\0';
    return AV_OK;
}

static av_status lit_hex(const char *kind, const uint8_t *b, size_t n, char *out, size_t cap, av_err *e)
{
    char hex[67];
    if (cap < strlen(kind) + 2 * n + 4)
        return too_long(e);
    if (n == 32)
        av_key_to_hex(b, hex);
    else {
        av_addr_to_hex(b, hex);
    }
    snprintf(out, cap, "%s(%s)", kind, hex);
    return AV_OK;
}

av_status av_qb_lit_key(const uint8_t key[32], char *out, size_t cap, av_err *e)
{
    return lit_hex("key", key, 32, out, cap, e);
}

av_status av_qb_lit_addr(const uint8_t addr[20], char *out, size_t cap, av_err *e)
{
    return lit_hex("addr", addr, 20, out, cap, e);
}

av_status av_qb_lit_u64(uint64_t v, char *out, size_t cap, av_err *e)
{
    int n = snprintf(out, cap, "u64(%llu)", (unsigned long long) v);
    return (n < 0 || (size_t) n >= cap) ? too_long(e) : AV_OK;
}

av_status av_qb_lit_i32(int32_t v, char *out, size_t cap, av_err *e)
{
    int n = snprintf(out, cap, "i32(%d)", (int) v);
    return (n < 0 || (size_t) n >= cap) ? too_long(e) : AV_OK;
}

av_status av_qb_lit_u256(const char *digits, char *out, size_t cap, av_err *e)
{
    size_t n = digits != NULL ? strlen(digits) : 0;
    if (n == 0 || n > 78)
        return invalid(e, "u256 digits");
    for (size_t i = 0; i < n; i++)
        if (digits[i] < '0' || digits[i] > '9')
            return invalid(e, "u256 digits");
    int w = snprintf(out, cap, "u256(%s)", digits);
    return (w < 0 || (size_t) w >= cap) ? too_long(e) : AV_OK;
}

av_status av_qb_lit_dec(const char *dec, char *out, size_t cap, av_err *e)
{
    size_t n = dec != NULL ? strlen(dec) : 0;
    size_t i = 0;
    size_t ip = 0;
    size_t fp = 0;
    if (n > 0 && dec[0] == '-')
        i = 1;
    for (; i < n && dec[i] >= '0' && dec[i] <= '9'; i++)
        ip++;
    if (i < n && dec[i] == '.') {
        i++;
        for (; i < n && dec[i] >= '0' && dec[i] <= '9'; i++)
            fp++;
    }
    if (i != n || ip == 0 || ip > 60 || fp > 18)
        return invalid(e, "dec literal");
    int w = snprintf(out, cap, "dec(%s)", dec);
    return (w < 0 || (size_t) w >= cap) ? too_long(e) : AV_OK;
}

bool av_qb_valid_name(const char *name)
{
    size_t n = name != NULL ? strlen(name) : 0;
    if (n == 0 || n > AV_ATTR_NAME_MAX)
        return false;
    bool first = (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z');
    if (!first)
        return false;
    for (size_t i = 1; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        if (!ok || (c == '-' && name[i - 1] == '-'))
            return false;
    }
    return true;
}

static bool valid_field(const char *name)
{
    return strcmp(name, "$key") == 0 || strcmp(name, "$owner") == 0 || strcmp(name, "$creator") == 0 ||
           strcmp(name, "$expiresAt") == 0 || av_qb_valid_name(name);
}

av_status av_qb_cmp(av_qb *q, const char *name, const char *op, const char *literal, av_err *e)
{
    static const char *const ops[] = {"=", "!=", "<", "<=", ">", ">="};
    bool op_ok = false;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        op_ok = op_ok || strcmp(ops[i], op) == 0;
    if (!op_ok)
        return invalid(e, "operator");
    if (!valid_field(name))
        return invalid(e, "attribute name");
    if (av_qb_raw(q, name, e) != AV_OK || av_qb_raw(q, " ", e) != AV_OK || av_qb_raw(q, op, e) != AV_OK ||
        av_qb_raw(q, " ", e) != AV_OK || av_qb_raw(q, literal, e) != AV_OK)
        return AV_ERR;
    return AV_OK;
}

static av_status or_chain(char *out, size_t cap, const char *field, const uint8_t *items, size_t item_len,
                          size_t n, av_err *e)
{
    av_qb q;
    char lit[AV_QB_LIT_MAX];
    if (n == 0)
        return invalid(e, "empty OR chain");
    av_qb_init(&q, out, cap);
    for (size_t i = 0; i < n; i++) {
        const uint8_t *it = items + i * item_len;
        av_status st =
            item_len == 32 ? av_qb_lit_key(it, lit, sizeof lit, e) : av_qb_lit_addr(it, lit, sizeof lit, e);
        if (st != AV_OK || av_qb_or(&q, e) != AV_OK || av_qb_cmp(&q, field, "=", lit, e) != AV_OK)
            return AV_ERR;
    }
    return AV_OK;
}

av_status av_qb_or_keys(char *out, size_t cap, const uint8_t (*keys)[32], size_t n, av_err *e)
{
    return or_chain(out, cap, "$key", &keys[0][0], 32, n, e);
}

av_status av_qb_or_creators(char *out, size_t cap, const uint8_t (*addrs)[20], size_t n, av_err *e)
{
    return or_chain(out, cap, "$creator", &addrs[0][0], 20, n, e);
}

av_status av_qb_paged(char *out, size_t cap, const char *base, const uint8_t cursor[32], av_err *e)
{
    av_qb q;
    char lit[AV_QB_LIT_MAX];
    av_qb_init(&q, out, cap);
    if (base != NULL && base[0] != '\0') {
        if (av_qb_raw(&q, "(", e) != AV_OK || av_qb_raw(&q, base, e) != AV_OK ||
            av_qb_raw(&q, ")", e) != AV_OK)
            return AV_ERR;
    }
    if (cursor != NULL) {
        if (av_qb_lit_key(cursor, lit, sizeof lit, e) != AV_OK || av_qb_and(&q, e) != AV_OK ||
            av_qb_cmp(&q, "$key", ">", lit, e) != AV_OK)
            return AV_ERR;
    } else if (q.len == 0) {
        /* match-all: every key is >= the zero key */
        static const uint8_t zero[32] = {0};
        if (av_qb_lit_key(zero, lit, sizeof lit, e) != AV_OK || av_qb_cmp(&q, "$key", ">=", lit, e) != AV_OK)
            return AV_ERR;
    }
    return av_qb_raw(&q, " ORDER BY $key ASC", e);
}
