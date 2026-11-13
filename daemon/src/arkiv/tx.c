/*
 * arkiv/tx.c: EIP-1559 transaction building, sighash computation, and signing serialization.
 */
#include "arkiv/tx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "arkiv/rawtx.h"
#include "arkiv/rlp.h"
#include "yyjson.h"

av_status av_tx_sighash(const av_tx_desc *tx, uint8_t out_hash[32], av_err *e)
{
    if (tx == NULL || out_hash == NULL) {
        av_err_set(e, "invalid_argument", "invalid arguments to av_tx_sighash");
        return AV_ERR;
    }

    av_rlp r;
    av_rlp_init(&r);
    av_rlp_list_begin(&r);
    av_rlp_u64(&r, tx->chain_id);
    av_rlp_u64(&r, tx->nonce);
    av_rlp_u64(&r, tx->max_priority_fee_per_gas);
    av_rlp_u64(&r, tx->max_fee_per_gas);
    av_rlp_u64(&r, tx->gas_limit);
    av_rlp_bytes(&r, tx->to, 20);
    av_rlp_u64(&r, 0); /* value = 0 */
    av_rlp_bytes(&r, tx->data, tx->data_len);
    av_rlp_list_begin(&r); /* accessList = [] */
    av_rlp_list_end(&r);
    av_rlp_list_end(&r);

    const uint8_t *rlp_data = NULL;
    size_t rlp_len = 0;
    if (av_rlp_finish(&r, &rlp_data, &rlp_len, e) != AV_OK) {
        av_rlp_free(&r);
        return AV_ERR;
    }

    uint8_t *payload = malloc(1 + rlp_len);
    if (payload == NULL) {
        av_rlp_free(&r);
        av_err_set(e, "internal", "out of memory for sighash buffer");
        return AV_ERR;
    }

    payload[0] = 0x02; /* EIP-1559 transaction type */
    memcpy(payload + 1, rlp_data, rlp_len);
    av_rlp_free(&r);

    av_keccak256(payload, 1 + rlp_len, out_hash);
    free(payload);
    return AV_OK;
}

av_status av_tx_encode_signed(const av_tx_desc *tx, uint8_t v_parity, const uint8_t r[32],
                              const uint8_t s[32], av_arena *arena, uint8_t **out_raw, size_t *out_len,
                              uint8_t out_tx_hash[32], av_err *e)
{
    if (tx == NULL || r == NULL || s == NULL || out_raw == NULL || out_len == NULL) {
        av_err_set(e, "invalid_argument", "invalid arguments to av_tx_encode_signed");
        return AV_ERR;
    }

    av_rlp rlp;
    av_rlp_init(&rlp);
    av_rlp_list_begin(&rlp);
    av_rlp_u64(&rlp, tx->chain_id);
    av_rlp_u64(&rlp, tx->nonce);
    av_rlp_u64(&rlp, tx->max_priority_fee_per_gas);
    av_rlp_u64(&rlp, tx->max_fee_per_gas);
    av_rlp_u64(&rlp, tx->gas_limit);
    av_rlp_bytes(&rlp, tx->to, 20);
    av_rlp_u64(&rlp, 0); /* value = 0 */
    av_rlp_bytes(&rlp, tx->data, tx->data_len);
    av_rlp_list_begin(&rlp); /* accessList = [] */
    av_rlp_list_end(&rlp);
    av_rlp_u64(&rlp, (uint64_t) v_parity);
    av_rlp_u256(&rlp, r);
    av_rlp_u256(&rlp, s);
    av_rlp_list_end(&rlp);

    const uint8_t *rlp_data = NULL;
    size_t rlp_len = 0;
    if (av_rlp_finish(&rlp, &rlp_data, &rlp_len, e) != AV_OK) {
        av_rlp_free(&rlp);
        return AV_ERR;
    }

    size_t total_len = 1 + rlp_len;
    uint8_t *res = av_arena_alloc(arena, total_len, 1);
    if (res == NULL) {
        av_rlp_free(&rlp);
        av_err_set(e, "internal", "arena out of memory for signed tx");
        return AV_ERR;
    }

    res[0] = 0x02; /* EIP-1559 */
    memcpy(res + 1, rlp_data, rlp_len);
    av_rlp_free(&rlp);

    *out_raw = res;
    *out_len = total_len;

    if (out_tx_hash != NULL) {
        av_keccak256(res, total_len, out_tx_hash);
    }

    return AV_OK;
}

bool av_tx_matches(const av_tx_desc *tx, const av_rawtx *rt)
{
    return rt->chain_id == tx->chain_id && rt->nonce == tx->nonce &&
           rt->max_priority_fee_per_gas == tx->max_priority_fee_per_gas &&
           rt->max_fee_per_gas == tx->max_fee_per_gas && rt->gas_limit == tx->gas_limit && rt->has_to &&
           memcmp(rt->to, tx->to, 20) == 0 && rt->value_zero && rt->data_len == tx->data_len &&
           (tx->data_len == 0 || memcmp(rt->data, tx->data, tx->data_len) == 0) && rt->n_access_list == 0;
}

/*
 * Remote signer: send the transaction as an eth_signTransaction / account_signTransaction object,
 * then decode what comes back and check that it is exactly that transaction, signed by the
 * configured address. A signer that changes any field, or signs with another key, is rejected.
 */
static av_status remote_sign(const av_tx_desc *tx, const av_signer *signer, av_arena *arena,
                             uint8_t **out_raw, size_t *out_len, uint8_t out_tx_hash[32], av_err *e)
{
    static const char hexd[] = "0123456789abcdef";
    char from_hex[43], to_hex[43];
    av_addr_to_hex(av_signer_get_address(signer), from_hex);
    av_addr_to_hex(tx->to, to_hex);
    size_t cap = tx->data_len * 2 + 512;
    char *params = av_arena_alloc(arena, cap, 1);
    if (params == NULL) {
        av_err_set(e, "internal", "out of memory for the signing request");
        return AV_ERR;
    }
    int n = snprintf(params, cap,
                     "[{\"from\":\"%s\",\"to\":\"%s\",\"gas\":\"0x%" PRIx64 "\",\"maxFeePerGas\":\"0x%" PRIx64
                     "\",\"maxPriorityFeePerGas\":\"0x%" PRIx64 "\",\"value\":\"0x0\",\"nonce\":\"0x%" PRIx64
                     "\",\"chainId\":\"0x%" PRIx64 "\",\"type\":\"0x2\",\"data\":\"0x",
                     from_hex, to_hex, tx->gas_limit, tx->max_fee_per_gas, tx->max_priority_fee_per_gas,
                     tx->nonce, tx->chain_id);
    if (n < 0 || (size_t) n + tx->data_len * 2 + 8 > cap) {
        av_err_set(e, "internal", "signing request too large");
        return AV_ERR;
    }
    size_t pos = (size_t) n;
    for (size_t i = 0; i < tx->data_len; i++) {
        params[pos++] = hexd[tx->data[i] >> 4];
        params[pos++] = hexd[tx->data[i] & 15];
    }
    memcpy(params + pos, "\"}]", 4);

    yyjson_doc *doc = NULL;
    yyjson_val *res = NULL;
    if (av_signer_remote_call(signer, av_signer_remote_method(signer), params, &doc, &res, e) != AV_OK)
        return AV_ERR;
    /* Web3Signer returns the raw transaction; geth and Clef return {"raw": "0x...", "tx": {...}}. */
    yyjson_val *rawv = yyjson_is_obj(res) ? yyjson_obj_get(res, "raw") : res;
    const char *raw_hex = yyjson_get_str(rawv);
    size_t raw_hex_len = yyjson_get_len(rawv);
    av_rawtx rt;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    av_status st = AV_ERR;
    if (raw_hex == NULL) {
        av_err_set(e, "write_disabled", "remote signer returned no raw transaction");
    } else if (av_rawtx_decode_hex(raw_hex, raw_hex_len, arena, &raw, &raw_len, &rt, e) != AV_OK) {
        char msg[sizeof(e->msg)];
        snprintf(msg, sizeof(msg), "%s", e->msg);
        av_err_set(e, "write_disabled", "remote signer returned an unusable transaction: %s", msg);
    } else if (!av_tx_matches(tx, &rt)) {
        av_err_set(e, "write_disabled", "remote signer changed the transaction it was asked to sign");
    } else if (memcmp(rt.sender, av_signer_get_address(signer), 20) != 0) {
        av_err_set(e, "write_disabled", "remote signer signed with a different address than configured");
    } else {
        *out_raw = raw;
        *out_len = raw_len;
        if (out_tx_hash != NULL)
            memcpy(out_tx_hash, rt.hash, 32);
        st = AV_OK;
    }
    yyjson_doc_free(doc);
    return st;
}

av_status av_tx_sign_and_encode(const av_tx_desc *tx, const av_signer *signer, av_arena *arena,
                                uint8_t **out_raw, size_t *out_len, uint8_t out_tx_hash[32], av_err *e)
{
    if (signer == NULL) {
        av_err_set(e, "invalid_argument", "signer required to sign transaction");
        return AV_ERR;
    }
    if (av_signer_get_kind(signer) == AV_SIGNER_REMOTE)
        return remote_sign(tx, signer, arena, out_raw, out_len, out_tx_hash, e);

    uint8_t sighash[32];
    if (av_tx_sighash(tx, sighash, e) != AV_OK)
        return AV_ERR;

    uint8_t r[32], s[32], v_parity = 0;
    if (av_signer_sign(signer, sighash, r, s, &v_parity, e) != AV_OK)
        return AV_ERR;

    return av_tx_encode_signed(tx, v_parity, r, s, arena, out_raw, out_len, out_tx_hash, e);
}
