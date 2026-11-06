/*
 * arkiv/tx.c: EIP-1559 transaction building, sighash computation, and signing serialization.
 */
#include "arkiv/tx.h"

#include <stdlib.h>
#include <string.h>

#include "arkiv/keccak.h"
#include "arkiv/rlp.h"

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

av_status av_tx_sign_and_encode(const av_tx_desc *tx, const av_signer *signer, av_arena *arena,
                                uint8_t **out_raw, size_t *out_len, uint8_t out_tx_hash[32], av_err *e)
{
    if (signer == NULL) {
        av_err_set(e, "invalid_argument", "signer required to sign transaction");
        return AV_ERR;
    }

    uint8_t sighash[32];
    if (av_tx_sighash(tx, sighash, e) != AV_OK)
        return AV_ERR;

    uint8_t r[32], s[32], v_parity = 0;
    if (av_signer_sign(signer, sighash, r, s, &v_parity, e) != AV_OK)
        return AV_ERR;

    return av_tx_encode_signed(tx, v_parity, r, s, arena, out_raw, out_len, out_tx_hash, e);
}
