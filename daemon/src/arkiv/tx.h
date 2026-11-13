/*
 * arkiv/tx.h: EIP-1559 transaction building, sighash computation, and signing serialization.
 */
#ifndef AV_ARKIV_TX_H
#define AV_ARKIV_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/rawtx.h"
#include "arkiv/signer.h"
#include "core/arena.h"
#include "log.h"

typedef struct {
    uint64_t chain_id;
    uint64_t nonce;
    uint64_t max_priority_fee_per_gas;
    uint64_t max_fee_per_gas;
    uint64_t gas_limit;
    uint8_t to[20];
    const uint8_t *data;
    size_t data_len;
} av_tx_desc;

/* Compute EIP-1559 transaction sighash: keccak256(0x02 || rlp([chainId, nonce, maxPriorityFee, maxFee, gasLimit, to, 0, data, []])) */
av_status av_tx_sighash(const av_tx_desc *tx, uint8_t out_hash[32], av_err *e);

/* Encode signed transaction: 0x02 || rlp([... yParity, r, s]) */
av_status av_tx_encode_signed(const av_tx_desc *tx, uint8_t v_parity, const uint8_t r[32],
                              const uint8_t s[32], av_arena *arena, uint8_t **out_raw, size_t *out_len,
                              uint8_t out_tx_hash[32], av_err *e);

/* True when the decoded signed transaction rt carries exactly the fields of tx (value 0, no access list). */
bool av_tx_matches(const av_tx_desc *tx, const av_rawtx *rt);

/* Sign and encode in one call. Local signers sign the sighash; a remote signer receives the whole
 * transaction and its answer is decoded and checked with av_tx_matches and the signer address. */
av_status av_tx_sign_and_encode(const av_tx_desc *tx, const av_signer *signer, av_arena *arena,
                                uint8_t **out_raw, size_t *out_len, uint8_t out_tx_hash[32], av_err *e);

#endif /* AV_ARKIV_TX_H */
