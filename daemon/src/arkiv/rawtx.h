/*
 * arkiv/rawtx.h: strict decoder for signed EIP-1559 (type 2) transactions and sender recovery.
 *
 * Used by POST /v1/collections/{name}/entities:submit (transactions signed by a client wallet) and
 * by the remote signer (arkiv/tx.c checks that what the signer returned is what was asked for).
 * Both inputs are untrusted: every length is checked against the input before it is read, RLP must
 * be canonical (no leading zeros, shortest length form), integers that must fit uint64 are bounded,
 * access-list entries must have the EIP-2930 shape [address, [storageKey, ...]],
 * the signature must have a low s (EIP-2), and the input must be exactly one transaction with no
 * trailing bytes. Fuzz target: test/fuzz/fuzz_rawtx.c.
 *
 * Ownership: av_rawtx.data points INTO the caller's raw buffer (no copy); keep the buffer alive
 * while the struct is used. Thread-safe; no shared state.
 */
#ifndef AV_ARKIV_RAWTX_H
#define AV_ARKIV_RAWTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "log.h"

/* A raw transaction larger than this is rejected before decoding (Arkiv's 128 KiB payload + margin). */
#define AV_RAWTX_MAX_BYTES ((size_t) 512 * 1024)

typedef struct {
    uint64_t chain_id;
    uint64_t nonce;
    uint64_t max_priority_fee_per_gas;
    uint64_t max_fee_per_gas;
    uint64_t gas_limit;
    bool has_to; /* false for contract creation (empty `to`) */
    uint8_t to[20];
    bool value_zero;
    const uint8_t *data; /* points into the raw buffer */
    size_t data_len;
    size_t n_access_list; /* number of access-list entries (not decoded further) */
    uint8_t y_parity;
    uint8_t r[32];
    uint8_t s[32];
    uint8_t sender[20]; /* recovered from the signature */
    uint8_t hash[32];   /* keccak256(raw): the transaction hash */
} av_rawtx;

/*
 * Decode and verify one signed type-2 transaction (0x02 || rlp([...])). On success every field of
 * out is set, including the recovered sender and the hash. Errors: "invalid_argument" with a short
 * reason (legacy/other types, bad RLP, oversize integers, bad signature).
 */
av_status av_rawtx_decode(const uint8_t *raw, size_t len, av_rawtx *out, av_err *e);

/* Same, from "0x"-prefixed (or bare) hex text; *raw_out receives the decoded bytes (arena memory). */
av_status av_rawtx_decode_hex(const char *hex, size_t hex_len, av_arena *a, uint8_t **raw_out,
                              size_t *raw_len, av_rawtx *out, av_err *e);

#endif /* AV_ARKIV_RAWTX_H */
