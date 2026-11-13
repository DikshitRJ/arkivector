/*
 * arkiv/rawtx.c: strict signed EIP-1559 transaction decoder and sender recovery. See rawtx.h.
 */
#include "arkiv/rawtx.h"

#include <stdlib.h>
#include <string.h>

#include "secp256k1.h"
#include "secp256k1_recovery.h"

#include "arkiv/keccak.h"
#include "codec/hex.h"

typedef struct {
    size_t start; /* payload start */
    size_t len;   /* payload length */
    size_t end;   /* offset just after the item */
    bool list;
} rlp_item;

/* Big-endian length of ll bytes at pos (no leading zero byte, ll in 1..8). */
static bool read_len(const uint8_t *b, size_t lim, size_t pos, size_t ll, size_t *out)
{
    if (ll == 0 || ll > 8 || pos > lim || ll > lim - pos || b[pos] == 0)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < ll; i++)
        v = (v << 8) | b[pos + i];
    if (v > (uint64_t) SIZE_MAX)
        return false;
    *out = (size_t) v;
    return true;
}

/* One canonical RLP item at pos, entirely inside [0, lim). */
static bool rlp_next(const uint8_t *b, size_t lim, size_t pos, rlp_item *it)
{
    if (pos >= lim)
        return false;
    uint8_t p = b[pos];
    size_t hdr = 1;
    size_t plen = 0;
    bool list = false;
    if (p < 0x80) {
        it->start = pos;
        it->len = 1;
        it->end = pos + 1;
        it->list = false;
        return true;
    }
    if (p <= 0xb7) {
        plen = (size_t) (p - 0x80);
    } else if (p <= 0xbf) {
        size_t ll = (size_t) (p - 0xb7);
        if (!read_len(b, lim, pos + 1, ll, &plen) || plen <= 55)
            return false;
        hdr = 1 + ll;
    } else if (p <= 0xf7) {
        plen = (size_t) (p - 0xc0);
        list = true;
    } else {
        size_t ll = (size_t) (p - 0xf7);
        if (!read_len(b, lim, pos + 1, ll, &plen) || plen <= 55)
            return false;
        hdr = 1 + ll;
        list = true;
    }
    if (hdr > lim - pos || plen > lim - pos - hdr)
        return false;
    /* A single byte below 0x80 must be encoded as itself. */
    if (!list && plen == 1 && b[pos + hdr] < 0x80)
        return false;
    it->start = pos + hdr;
    it->len = plen;
    it->end = pos + hdr + plen;
    it->list = list;
    return true;
}

/* Canonical unsigned integer that fits in uint64 (empty string = 0, no leading zero byte). */
static bool item_u64(const uint8_t *b, const rlp_item *it, uint64_t *out)
{
    if (it->list || it->len > 8 || (it->len > 0 && b[it->start] == 0))
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < it->len; i++)
        v = (v << 8) | b[it->start + i];
    *out = v;
    return true;
}

/* Canonical unsigned 256-bit integer, left-padded to 32 bytes. */
static bool item_u256(const uint8_t *b, const rlp_item *it, uint8_t out[32])
{
    if (it->list || it->len > 32 || (it->len > 0 && b[it->start] == 0))
        return false;
    memset(out, 0, 32);
    if (it->len > 0)
        memcpy(out + (32 - it->len), b + it->start, it->len);
    return true;
}

/* One EIP-2930 access-list entry at pos: [address (20 bytes), [storageKey (32 bytes), ...]]. */
static bool access_entry_ok(const uint8_t *b, size_t lim, size_t pos, size_t *next)
{
    rlp_item entry, addr, keys;
    if (!rlp_next(b, lim, pos, &entry) || !entry.list)
        return false;
    if (!rlp_next(b, entry.end, entry.start, &addr) || addr.list || addr.len != 20)
        return false;
    if (!rlp_next(b, entry.end, addr.end, &keys) || !keys.list || keys.end != entry.end)
        return false;
    for (size_t k = keys.start; k < keys.end;) {
        rlp_item key;
        if (!rlp_next(b, keys.end, k, &key) || key.list || key.len != 32)
            return false;
        k = key.end;
    }
    *next = entry.end;
    return true;
}

static bool is_zero32(const uint8_t v[32])
{
    for (size_t i = 0; i < 32; i++)
        if (v[i] != 0)
            return false;
    return true;
}

/* s <= n/2 (EIP-2): n/2 = 7fffffff ffffffff ffffffff ffffffff 5d576e73 57a4501d dfe92f46 681b20a0 */
static bool low_s(const uint8_t s[32])
{
    static const uint8_t half_n[32] = {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4,
                                       0x50, 0x1d, 0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0};
    return memcmp(s, half_n, 32) <= 0;
}

static av_status bad(av_err *e, const char *why)
{
    av_err_set(e, "invalid_argument", "signed transaction: %s", why);
    return AV_ERR;
}

av_status av_rawtx_decode(const uint8_t *raw, size_t len, av_rawtx *out, av_err *e)
{
    memset(out, 0, sizeof(*out));
    if (raw == NULL || len == 0)
        return bad(e, "empty");
    if (len > AV_RAWTX_MAX_BYTES)
        return bad(e, "too large");
    if (raw[0] != 0x02) {
        if (raw[0] >= 0xc0)
            return bad(e, "legacy (type 0) transactions are not accepted; sign as EIP-1559 (type 2)");
        return bad(e, "only EIP-1559 (type 2) transactions are accepted");
    }

    rlp_item outer;
    if (!rlp_next(raw, len, 1, &outer) || !outer.list)
        return bad(e, "payload is not an RLP list");
    if (outer.end != len)
        return bad(e, "trailing bytes after the transaction");

    rlp_item f[12];
    size_t n = 0;
    size_t pos = outer.start;
    while (pos < outer.end) {
        if (n == 12)
            return bad(e, "too many fields (expected 12)");
        if (!rlp_next(raw, outer.end, pos, &f[n]))
            return bad(e, "malformed RLP field");
        pos = f[n].end;
        n++;
    }
    if (n != 12)
        return bad(e, "wrong number of fields (expected 12)");

    if (!item_u64(raw, &f[0], &out->chain_id) || !item_u64(raw, &f[1], &out->nonce) ||
        !item_u64(raw, &f[2], &out->max_priority_fee_per_gas) ||
        !item_u64(raw, &f[3], &out->max_fee_per_gas) || !item_u64(raw, &f[4], &out->gas_limit))
        return bad(e, "chainId, nonce, fees and gas must be canonical integers below 2^64");
    if (f[5].list || (f[5].len != 0 && f[5].len != 20))
        return bad(e, "'to' must be a 20-byte address");
    out->has_to = f[5].len == 20;
    if (out->has_to)
        memcpy(out->to, raw + f[5].start, 20);
    uint8_t value[32];
    if (!item_u256(raw, &f[6], value))
        return bad(e, "'value' is not a canonical integer");
    out->value_zero = is_zero32(value);
    if (f[7].list)
        return bad(e, "'data' must be a byte string");
    out->data = raw + f[7].start;
    out->data_len = f[7].len;
    if (!f[8].list)
        return bad(e, "'accessList' must be a list");
    for (size_t p = f[8].start; p < f[8].end;) {
        if (!access_entry_ok(raw, f[8].end, p, &p))
            return bad(e, "malformed access list (EIP-2930: [address, [storageKey, ...]])");
        out->n_access_list++;
    }
    uint64_t parity = 0;
    if (!item_u64(raw, &f[9], &parity) || parity > 1)
        return bad(e, "yParity must be 0 or 1");
    out->y_parity = (uint8_t) parity;
    if (!item_u256(raw, &f[10], out->r) || !item_u256(raw, &f[11], out->s))
        return bad(e, "r and s must be canonical 256-bit integers");
    if (is_zero32(out->r) || is_zero32(out->s))
        return bad(e, "zero signature component");
    if (!low_s(out->s))
        return bad(e, "signature s is not in the lower half of the curve order (EIP-2)");
    if (out->max_priority_fee_per_gas > out->max_fee_per_gas)
        return bad(e, "maxPriorityFeePerGas exceeds maxFeePerGas");

    /* Sighash: keccak256(0x02 || rlp([fields 0..8])). The fields are contiguous in the input. */
    size_t span_start = outer.start;
    size_t span_len = f[8].end - span_start;
    uint8_t hdr[9];
    size_t hdr_len = 0;
    if (span_len <= 55) {
        hdr[hdr_len++] = (uint8_t) (0xc0 + span_len);
    } else {
        uint8_t lb[8];
        size_t nb = 0;
        for (size_t v = span_len; v > 0; v >>= 8)
            lb[nb++] = (uint8_t) (v & 0xff);
        hdr[hdr_len++] = (uint8_t) (0xf7 + nb);
        while (nb > 0)
            hdr[hdr_len++] = lb[--nb];
    }
    uint8_t *pre = malloc(1 + hdr_len + span_len);
    if (pre == NULL) {
        av_err_set(e, "internal", "out of memory decoding a transaction");
        return AV_ERR;
    }
    pre[0] = 0x02;
    memcpy(pre + 1, hdr, hdr_len);
    memcpy(pre + 1 + hdr_len, raw + span_start, span_len);
    uint8_t sighash[32];
    av_keccak256(pre, 1 + hdr_len + span_len, sighash);
    free(pre);

    uint8_t compact[64];
    memcpy(compact, out->r, 32);
    memcpy(compact + 32, out->s, 32);
    secp256k1_ecdsa_recoverable_signature sig;
    secp256k1_pubkey pub;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(secp256k1_context_static, &sig, compact,
                                                             (int) out->y_parity) ||
        !secp256k1_ecdsa_recover(secp256k1_context_static, &pub, &sig, sighash))
        return bad(e, "signature does not recover to a public key");
    uint8_t pub65[65];
    size_t pub_len = sizeof(pub65);
    if (!secp256k1_ec_pubkey_serialize(secp256k1_context_static, pub65, &pub_len, &pub,
                                       SECP256K1_EC_UNCOMPRESSED) ||
        pub_len != 65)
        return bad(e, "cannot serialize the recovered public key");
    uint8_t ph[32];
    av_keccak256(pub65 + 1, 64, ph);
    memcpy(out->sender, ph + 12, 20);
    av_keccak256(raw, len, out->hash);
    return AV_OK;
}

av_status av_rawtx_decode_hex(const char *hex, size_t hex_len, av_arena *a, uint8_t **raw_out,
                              size_t *raw_len, av_rawtx *out, av_err *e)
{
    *raw_out = NULL;
    *raw_len = 0;
    if (hex == NULL || hex_len > 2 + 2 * AV_RAWTX_MAX_BYTES)
        return bad(e, "too large");
    uint8_t *raw = NULL;
    size_t n = 0;
    if (av_hex_decode(hex, hex_len, a, &raw, &n, e) != AV_OK)
        return bad(e, "not valid hex");
    if (av_rawtx_decode(raw, n, out, e) != AV_OK)
        return AV_ERR;
    *raw_out = raw;
    *raw_len = n;
    return AV_OK;
}
