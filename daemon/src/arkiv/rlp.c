/*
 * arkiv/rlp.c: RLP encoder. See rlp.h.
 */
#include "arkiv/rlp.h"

#include <stdlib.h>
#include <string.h>

void av_rlp_init(av_rlp *r)
{
    memset(r, 0, sizeof(*r));
}

void av_rlp_free(av_rlp *r)
{
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

static int reserve(av_rlp *r, size_t extra)
{
    if (r->failed) {
        return -1;
    }
    if (extra > SIZE_MAX - r->len) {
        r->failed = 1;
        return -1;
    }
    size_t need = r->len + extra;
    if (need <= r->cap) {
        return 0;
    }
    size_t cap = r->cap ? r->cap : 256;
    while (cap < need) {
        cap *= 2;
    }
    uint8_t *b = realloc(r->buf, cap);
    if (b == NULL) {
        r->failed = 1;
        return -1;
    }
    r->buf = b;
    r->cap = cap;
    return 0;
}

static size_t be_len(uint64_t v)
{
    size_t n = 0;
    while (v != 0) {
        n++;
        v >>= 8;
    }
    return n;
}

static void put_be(uint8_t *dst, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[n - 1 - i] = (uint8_t) (v >> (8 * i));
    }
}

/* Header for a payload of length n with short offset (0x80 strings, 0xc0 lists). */
static void header(av_rlp *r, size_t n, uint8_t offset)
{
    if (n < 56) {
        if (reserve(r, 1) == 0) {
            r->buf[r->len++] = (uint8_t) (offset + n);
        }
        return;
    }
    size_t ll = be_len((uint64_t) n);
    if (reserve(r, 1 + ll) == 0) {
        r->buf[r->len++] = (uint8_t) (offset + 55 + ll);
        put_be(r->buf + r->len, (uint64_t) n, ll);
        r->len += ll;
    }
}

void av_rlp_bytes(av_rlp *r, const uint8_t *data, size_t n)
{
    if (n == 1 && data[0] < 0x80) {
        if (reserve(r, 1) == 0) {
            r->buf[r->len++] = data[0];
        }
        return;
    }
    header(r, n, 0x80);
    if (n > 0 && reserve(r, n) == 0) {
        memcpy(r->buf + r->len, data, n);
        r->len += n;
    }
}

void av_rlp_u64(av_rlp *r, uint64_t v)
{
    uint8_t tmp[8];
    size_t n = be_len(v);
    put_be(tmp, v, n);
    av_rlp_bytes(r, tmp, n);
}

void av_rlp_u256(av_rlp *r, const uint8_t be32[32])
{
    size_t i = 0;
    while (i < 32 && be32[i] == 0) {
        i++;
    }
    av_rlp_bytes(r, be32 + i, 32 - i);
}

void av_rlp_list_begin(av_rlp *r)
{
    if (r->depth >= AV_RLP_MAX_DEPTH) {
        r->failed = 1;
        return;
    }
    r->open[r->depth++] = r->len;
}

void av_rlp_list_end(av_rlp *r)
{
    if (r->depth == 0 || r->failed) {
        r->failed = 1;
        return;
    }
    size_t start = r->open[--r->depth];
    size_t n = r->len - start;
    size_t hl = n < 56 ? 1 : 1 + be_len((uint64_t) n);
    if (reserve(r, hl) != 0) {
        return;
    }
    memmove(r->buf + start + hl, r->buf + start, n);
    if (n < 56) {
        r->buf[start] = (uint8_t) (0xc0 + n);
    } else {
        size_t ll = hl - 1;
        r->buf[start] = (uint8_t) (0xc0 + 55 + ll);
        put_be(r->buf + start + 1, (uint64_t) n, ll);
    }
    r->len += hl;
}

av_status av_rlp_finish(av_rlp *r, const uint8_t **out, size_t *len, av_err *e)
{
    if (r->failed || r->depth != 0) {
        av_err_set(e, "internal", "rlp: %s", r->failed ? "allocation failure or misuse" : "unclosed list");
        return AV_ERR;
    }
    *out = r->buf;
    *len = r->len;
    return AV_OK;
}
