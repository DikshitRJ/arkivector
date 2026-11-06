/*
 * arkiv/abi.h: Arkiv execute((uint8,bytes)[]) ABI encoder and decoder,
 * error catalogue decoder, and key prediction.
 */
#ifndef AV_ARKIV_ABI_H
#define AV_ARKIV_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"
#include "core/arena.h"
#include "log.h"

#define AV_ARKIV_EXECUTE_SELECTOR 0x49650044
#define AV_ARKIV_CONTRACT_ADDR    "0x4400000000000000000000000000000000000044"

typedef enum {
    AV_OP_CREATE = 1,
    AV_OP_PATCH = 2,
    AV_OP_EXTEND_EXPIRY = 3,
    AV_OP_TRANSFER_OWNERSHIP = 4,
    AV_OP_DELETE = 5
} av_op_type;

typedef struct {
    av_op_type type;
    union {
        struct {
            uint8_t salt[16]; /* uint128 */
            uint64_t expires_at;
            uint64_t min_lifetime;
            uint8_t creation_flags; /* 0x01 RO, 0x02 EXT */
            av_attr *attributes;
            size_t n_attributes;
        } create;
        struct {
            uint8_t entity_key[32];
            av_attr *mutations;
            size_t n_mutations;
        } patch;
        struct {
            uint8_t entity_key[32];
            uint64_t expires_at;
            uint64_t min_lifetime;
        } extend;
        struct {
            uint8_t entity_key[32];
            uint8_t new_owner[20];
        } transfer;
        struct {
            uint8_t entity_key[32];
        } del;
    } u;
} av_mutation_op;

/* Revert error decoding from the 22-error catalogue */
const char *av_abi_decode_revert(const uint8_t *data, size_t len);
const char *av_abi_revert_error_name(const uint8_t *data, size_t len);

/* Encode execute((uint8,bytes)[]) */
av_status av_abi_encode_execute(const av_mutation_op *ops, size_t n_ops, av_arena *arena,
                                uint8_t **out_calldata, size_t *out_len, av_err *e);

/* Decode execute((uint8,bytes)[]) */
av_status av_abi_decode_execute(const uint8_t *calldata, size_t len, av_arena *arena,
                                av_mutation_op **out_ops, size_t *out_n_ops, av_err *e);

/* Deterministic entity key derivation */
void av_abi_predict_entity_key(uint64_t chain_id, const uint8_t contract_addr[20], const uint8_t owner[20],
                               uint64_t nonce, const uint8_t salt[16], uint8_t out_key[32]);

#endif /* AV_ARKIV_ABI_H */
