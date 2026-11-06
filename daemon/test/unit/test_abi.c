/*
 * test_abi.c: unit tests for Arkiv execute((uint8,bytes)[]) ABI encoding/decoding,
 * revert error catalogue decoding, and deterministic key prediction.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/abi.h"
#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "core/arena.h"
#include "greatest.h"

TEST test_revert_catalog_decoding(void)
{
    static const struct {
        uint8_t sel[4];
        const char *name;
    } catalogue[] = {
        {{0xc2, 0xe5, 0x34, 0x7d}, "EmptyBatch"},
        {{0x6b, 0xd8, 0x47, 0xc6}, "InvalidOpType"},
        {{0x89, 0xbc, 0x94, 0x48}, "NonCanonicalOperationData"},
        {{0x32, 0x06, 0x84, 0x6a}, "AttributesNotSorted"},
        {{0xd7, 0xce, 0x98, 0x1c}, "TooManyAttributes"},
        {{0x78, 0xa5, 0x85, 0x38}, "EmptyMutations"},
        {{0x42, 0x50, 0x1b, 0xac}, "TombstoneInCreate"},
        {{0x01, 0x9f, 0x59, 0xdf}, "TombstoneValueNotEmpty"},
        {{0x79, 0xb2, 0xb3, 0x98}, "SystemAttributeNotWritable"},
        {{0x63, 0x9f, 0xb7, 0xab}, "InvalidValueType"},
        {{0xc1, 0x09, 0xae, 0xf4}, "Ident32Empty"},
        {{0x27, 0x6f, 0x77, 0x98}, "Ident32InvalidByte"},
        {{0x66, 0xa8, 0x9f, 0x2a}, "EntityNotFound"},
        {{0x24, 0x11, 0xcf, 0x56}, "EntityExpired"},
        {{0xa3, 0x22, 0x1e, 0xf2}, "NotOwner"},
        {{0xd8, 0x58, 0xa8, 0x24}, "ReadOnlyEntity"},
        {{0xdc, 0xc1, 0x86, 0x11}, "ReservedCreationFlags"},
        {{0x26, 0x04, 0x9d, 0x1c}, "ExpiryDeadOnArrival"},
        {{0xdd, 0x7e, 0x98, 0x6b}, "ExpiryBeyondMaxLifetime"},
        {{0x33, 0x26, 0x8a, 0x0d}, "ExpiryNotExtended"},
        {{0x87, 0x42, 0xc6, 0x7c}, "TransferToZeroAddress"},
        {{0xe3, 0x24, 0xb1, 0x69}, "TransferToSelf"},
    };

    size_t count = sizeof(catalogue) / sizeof(catalogue[0]);
    ASSERT_EQ(22, (int) count);

    for (size_t i = 0; i < count; i++) {
        const char *name1 = av_abi_decode_revert(catalogue[i].sel, 4);
        ASSERT(name1 != NULL);
        ASSERT_STR_EQ(catalogue[i].name, name1);

        const char *name2 = av_abi_revert_error_name(catalogue[i].sel, 4);
        ASSERT(name2 != NULL);
        ASSERT_STR_EQ(catalogue[i].name, name2);

        /* Also test with simulated trailing calldata argument bytes */
        uint8_t payload[36];
        memcpy(payload, catalogue[i].sel, 4);
        memset(payload + 4, 0xaa, 32);
        const char *name_long = av_abi_decode_revert(payload, sizeof(payload));
        ASSERT(name_long != NULL);
        ASSERT_STR_EQ(catalogue[i].name, name_long);
    }

    /* Invalid or unknown selectors */
    static const uint8_t unknown_sel[4] = {0x00, 0x11, 0x22, 0x33};
    const char *unknown = av_abi_decode_revert(unknown_sel, 4);
    ASSERT(unknown == NULL || strcmp(unknown, "Unknown") == 0);

    /* Too short data (< 4 bytes) */
    static const uint8_t short_sel[3] = {0xc2, 0xe5, 0x34};
    const char *short_res = av_abi_decode_revert(short_sel, 3);
    ASSERT(short_res == NULL || strcmp(short_res, "Unknown") == 0);

    PASS();
}

TEST test_abi_create_op(void)
{
    av_arena *arena = av_arena_create("test_abi_create", 65536, 0);
    ASSERT(arena != NULL);

    av_err err;
    av_err_clear(&err);

    /* Prepare 3 sorted attributes: "age", "name", "verified" */
    av_attr attrs[3];
    memset(attrs, 0, sizeof(attrs));

    /* 1. "age": i32 */
    strncpy(attrs[0].name, "age", sizeof(attrs[0].name) - 1);
    attrs[0].type = AV_AT_I32;
    attrs[0].indexed = true;
    uint8_t val_age[32] = {0};
    val_age[31] = 25;
    attrs[0].val = val_age;
    attrs[0].len = 32;

    /* 2. "name": str */
    strncpy(attrs[1].name, "name", sizeof(attrs[1].name) - 1);
    attrs[1].type = AV_AT_STR;
    attrs[1].indexed = true;
    static const uint8_t val_name[] = "Charlie";
    attrs[1].val = val_name;
    attrs[1].len = strlen("Charlie");

    /* 3. "verified": bool */
    strncpy(attrs[2].name, "verified", sizeof(attrs[2].name) - 1);
    attrs[2].type = AV_AT_BOOL;
    attrs[2].indexed = false;
    uint8_t val_verified[32] = {0};
    val_verified[31] = 1;
    attrs[2].val = val_verified;
    attrs[2].len = 32;

    av_mutation_op op;
    memset(&op, 0, sizeof(op));
    op.type = AV_OP_CREATE;
    for (int i = 0; i < 16; i++) {
        op.u.create.salt[i] = (uint8_t) (i + 1);
    }
    op.u.create.expires_at = 1234567ULL;
    op.u.create.min_lifetime = 60000ULL;
    op.u.create.creation_flags = 0x02; /* Permissionless Extension */
    op.u.create.attributes = attrs;
    op.u.create.n_attributes = 3;

    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    av_status st = av_abi_encode_execute(&op, 1, arena, &calldata, &calldata_len, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(calldata != NULL);
    ASSERT(calldata_len >= 4);

    /* Verify 4-byte selector 0x49650044 */
    ASSERT_EQ(0x49, calldata[0]);
    ASSERT_EQ(0x65, calldata[1]);
    ASSERT_EQ(0x00, calldata[2]);
    ASSERT_EQ(0x44, calldata[3]);

    /* Decode */
    av_mutation_op *decoded = NULL;
    size_t n_decoded = 0;
    st = av_abi_decode_execute(calldata, calldata_len, arena, &decoded, &n_decoded, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(1, (int) n_decoded);
    ASSERT(decoded != NULL);

    ASSERT_EQ(AV_OP_CREATE, decoded[0].type);
    ASSERT_EQ(0, memcmp(op.u.create.salt, decoded[0].u.create.salt, 16));
    ASSERT_EQ(op.u.create.expires_at, decoded[0].u.create.expires_at);
    ASSERT_EQ(op.u.create.min_lifetime, decoded[0].u.create.min_lifetime);
    ASSERT_EQ(op.u.create.creation_flags, decoded[0].u.create.creation_flags);
    ASSERT_EQ(3, (int) decoded[0].u.create.n_attributes);

    for (size_t i = 0; i < 3; i++) {
        ASSERT_STR_EQ(attrs[i].name, decoded[0].u.create.attributes[i].name);
        ASSERT_EQ(attrs[i].type, decoded[0].u.create.attributes[i].type);
        ASSERT_EQ(attrs[i].indexed, decoded[0].u.create.attributes[i].indexed);
        ASSERT_EQ(attrs[i].len, decoded[0].u.create.attributes[i].len);
        ASSERT_EQ(0, memcmp(attrs[i].val, decoded[0].u.create.attributes[i].val, attrs[i].len));
    }

    av_arena_destroy(arena);
    PASS();
}

TEST test_abi_patch_op(void)
{
    av_arena *arena = av_arena_create("test_abi_patch", 65536, 0);
    ASSERT(arena != NULL);

    av_err err;
    av_err_clear(&err);

    av_attr mutations[2];
    memset(mutations, 0, sizeof(mutations));

    /* 1. "city": str */
    strncpy(mutations[0].name, "city", sizeof(mutations[0].name) - 1);
    mutations[0].type = AV_AT_STR;
    mutations[0].indexed = true;
    static const uint8_t val_city[] = "Prague";
    mutations[0].val = val_city;
    mutations[0].len = strlen("Prague");

    /* 2. "old_field": tombstone */
    strncpy(mutations[1].name, "old_field", sizeof(mutations[1].name) - 1);
    mutations[1].type = AV_AT_TOMBSTONE;
    mutations[1].indexed = false;
    mutations[1].val = NULL;
    mutations[1].len = 0;

    av_mutation_op op;
    memset(&op, 0, sizeof(op));
    op.type = AV_OP_PATCH;
    memset(op.u.patch.entity_key, 0x42, 32);
    op.u.patch.mutations = mutations;
    op.u.patch.n_mutations = 2;

    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    av_status st = av_abi_encode_execute(&op, 1, arena, &calldata, &calldata_len, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(calldata != NULL);

    av_mutation_op *decoded = NULL;
    size_t n_decoded = 0;
    st = av_abi_decode_execute(calldata, calldata_len, arena, &decoded, &n_decoded, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(1, (int) n_decoded);
    ASSERT_EQ(AV_OP_PATCH, decoded[0].type);
    ASSERT_EQ(0, memcmp(op.u.patch.entity_key, decoded[0].u.patch.entity_key, 32));
    ASSERT_EQ(2, (int) decoded[0].u.patch.n_mutations);

    ASSERT_STR_EQ("city", decoded[0].u.patch.mutations[0].name);
    ASSERT_EQ(AV_AT_STR, decoded[0].u.patch.mutations[0].type);
    ASSERT_EQ(true, decoded[0].u.patch.mutations[0].indexed);
    ASSERT_EQ(strlen("Prague"), decoded[0].u.patch.mutations[0].len);
    ASSERT_EQ(0, memcmp("Prague", decoded[0].u.patch.mutations[0].val, strlen("Prague")));

    ASSERT_STR_EQ("old_field", decoded[0].u.patch.mutations[1].name);
    ASSERT_EQ(AV_AT_TOMBSTONE, decoded[0].u.patch.mutations[1].type);
    ASSERT_EQ(0, (int) decoded[0].u.patch.mutations[1].len);

    av_arena_destroy(arena);
    PASS();
}

TEST test_abi_extend_expiry_op(void)
{
    av_arena *arena = av_arena_create("test_abi_extend", 65536, 0);
    ASSERT(arena != NULL);

    av_err err;
    av_err_clear(&err);

    av_mutation_op op;
    memset(&op, 0, sizeof(op));
    op.type = AV_OP_EXTEND_EXPIRY;
    memset(op.u.extend.entity_key, 0x77, 32);
    op.u.extend.expires_at = 999999ULL;
    op.u.extend.min_lifetime = 10000ULL;

    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    av_status st = av_abi_encode_execute(&op, 1, arena, &calldata, &calldata_len, &err);
    ASSERT_EQ(AV_OK, st);

    av_mutation_op *decoded = NULL;
    size_t n_decoded = 0;
    st = av_abi_decode_execute(calldata, calldata_len, arena, &decoded, &n_decoded, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(1, (int) n_decoded);
    ASSERT_EQ(AV_OP_EXTEND_EXPIRY, decoded[0].type);
    ASSERT_EQ(0, memcmp(op.u.extend.entity_key, decoded[0].u.extend.entity_key, 32));
    ASSERT_EQ(999999ULL, decoded[0].u.extend.expires_at);
    ASSERT_EQ(10000ULL, decoded[0].u.extend.min_lifetime);

    av_arena_destroy(arena);
    PASS();
}

TEST test_abi_delete_op(void)
{
    av_arena *arena = av_arena_create("test_abi_del", 65536, 0);
    ASSERT(arena != NULL);

    av_err err;
    av_err_clear(&err);

    av_mutation_op op;
    memset(&op, 0, sizeof(op));
    op.type = AV_OP_DELETE;
    memset(op.u.del.entity_key, 0x99, 32);

    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    av_status st = av_abi_encode_execute(&op, 1, arena, &calldata, &calldata_len, &err);
    ASSERT_EQ(AV_OK, st);

    av_mutation_op *decoded = NULL;
    size_t n_decoded = 0;
    st = av_abi_decode_execute(calldata, calldata_len, arena, &decoded, &n_decoded, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(1, (int) n_decoded);
    ASSERT_EQ(AV_OP_DELETE, decoded[0].type);
    ASSERT_EQ(0, memcmp(op.u.del.entity_key, decoded[0].u.del.entity_key, 32));

    av_arena_destroy(arena);
    PASS();
}

TEST test_abi_roundtrip_batch(void)
{
    av_arena *arena = av_arena_create("test_abi_roundtrip", 131072, 0);
    ASSERT(arena != NULL);

    av_err err;
    av_err_clear(&err);

    av_attr c_attrs[1];
    memset(c_attrs, 0, sizeof(c_attrs));
    strncpy(c_attrs[0].name, "label", sizeof(c_attrs[0].name) - 1);
    c_attrs[0].type = AV_AT_STR;
    c_attrs[0].indexed = true;
    static const uint8_t val_lbl[] = "hello";
    c_attrs[0].val = val_lbl;
    c_attrs[0].len = strlen("hello");

    av_attr p_attrs[1];
    memset(p_attrs, 0, sizeof(p_attrs));
    strncpy(p_attrs[0].name, "count", sizeof(p_attrs[0].name) - 1);
    p_attrs[0].type = AV_AT_I32;
    p_attrs[0].indexed = true;
    uint8_t val_cnt[32] = {0};
    val_cnt[31] = 42;
    p_attrs[0].val = val_cnt;
    p_attrs[0].len = 32;

    av_mutation_op ops[4];
    memset(ops, 0, sizeof(ops));

    /* Op 0: CREATE */
    ops[0].type = AV_OP_CREATE;
    memset(ops[0].u.create.salt, 0x12, 16);
    ops[0].u.create.expires_at = 8888ULL;
    ops[0].u.create.min_lifetime = 1200ULL;
    ops[0].u.create.creation_flags = 0x01; /* Read Only */
    ops[0].u.create.attributes = c_attrs;
    ops[0].u.create.n_attributes = 1;

    /* Op 1: PATCH */
    ops[1].type = AV_OP_PATCH;
    memset(ops[1].u.patch.entity_key, 0xaa, 32);
    ops[1].u.patch.mutations = p_attrs;
    ops[1].u.patch.n_mutations = 1;

    /* Op 2: EXTEND_EXPIRY */
    ops[2].type = AV_OP_EXTEND_EXPIRY;
    memset(ops[2].u.extend.entity_key, 0xbb, 32);
    ops[2].u.extend.expires_at = 99999ULL;
    ops[2].u.extend.min_lifetime = 500ULL;

    /* Op 3: DELETE */
    ops[3].type = AV_OP_DELETE;
    memset(ops[3].u.del.entity_key, 0xcc, 32);

    uint8_t *calldata = NULL;
    size_t calldata_len = 0;
    av_status st = av_abi_encode_execute(ops, 4, arena, &calldata, &calldata_len, &err);
    ASSERT_EQ(AV_OK, st);

    av_mutation_op *decoded = NULL;
    size_t n_decoded = 0;
    st = av_abi_decode_execute(calldata, calldata_len, arena, &decoded, &n_decoded, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(4, (int) n_decoded);

    /* Verify Op 0 */
    ASSERT_EQ(AV_OP_CREATE, decoded[0].type);
    ASSERT_EQ(0, memcmp(ops[0].u.create.salt, decoded[0].u.create.salt, 16));
    ASSERT_EQ(ops[0].u.create.expires_at, decoded[0].u.create.expires_at);
    ASSERT_EQ(ops[0].u.create.min_lifetime, decoded[0].u.create.min_lifetime);
    ASSERT_EQ(ops[0].u.create.creation_flags, decoded[0].u.create.creation_flags);
    ASSERT_EQ(1, (int) decoded[0].u.create.n_attributes);
    ASSERT_STR_EQ("label", decoded[0].u.create.attributes[0].name);
    ASSERT_EQ(AV_AT_STR, decoded[0].u.create.attributes[0].type);
    ASSERT_EQ(true, decoded[0].u.create.attributes[0].indexed);
    ASSERT_EQ(5, (int) decoded[0].u.create.attributes[0].len);
    ASSERT_EQ(0, memcmp("hello", decoded[0].u.create.attributes[0].val, 5));

    /* Verify Op 1 */
    ASSERT_EQ(AV_OP_PATCH, decoded[1].type);
    ASSERT_EQ(0, memcmp(ops[1].u.patch.entity_key, decoded[1].u.patch.entity_key, 32));
    ASSERT_EQ(1, (int) decoded[1].u.patch.n_mutations);
    ASSERT_STR_EQ("count", decoded[1].u.patch.mutations[0].name);
    ASSERT_EQ(AV_AT_I32, decoded[1].u.patch.mutations[0].type);
    ASSERT_EQ(true, decoded[1].u.patch.mutations[0].indexed);
    ASSERT_EQ(32, (int) decoded[1].u.patch.mutations[0].len);
    ASSERT_EQ(0, memcmp(val_cnt, decoded[1].u.patch.mutations[0].val, 32));

    /* Verify Op 2 */
    ASSERT_EQ(AV_OP_EXTEND_EXPIRY, decoded[2].type);
    ASSERT_EQ(0, memcmp(ops[2].u.extend.entity_key, decoded[2].u.extend.entity_key, 32));
    ASSERT_EQ(ops[2].u.extend.expires_at, decoded[2].u.extend.expires_at);
    ASSERT_EQ(ops[2].u.extend.min_lifetime, decoded[2].u.extend.min_lifetime);

    /* Verify Op 3 */
    ASSERT_EQ(AV_OP_DELETE, decoded[3].type);
    ASSERT_EQ(0, memcmp(ops[3].u.del.entity_key, decoded[3].u.del.entity_key, 32));

    av_arena_destroy(arena);
    PASS();
}

TEST test_abi_predict_key(void)
{
    uint64_t chain_id = 7794095;
    uint8_t contract_addr[20];
    memset(contract_addr, 0x44, 20);
    uint8_t owner[20];
    memset(owner, 0x11, 20);
    uint64_t nonce = 42;
    uint8_t salt[16];
    memset(salt, 0x7a, 16);

    uint8_t predicted_key[32];
    av_abi_predict_entity_key(chain_id, contract_addr, owner, nonce, salt, predicted_key);

    /* Verify against reference keccak256 packed derivation */
    uint8_t packed[96];
    memset(packed, 0, 96);
    /* chain_id as 32-byte big endian */
    for (int i = 0; i < 8; i++) {
        packed[31 - i] = (uint8_t) ((chain_id >> (8 * i)) & 0xff);
    }
    memcpy(packed + 32, contract_addr, 20);
    memcpy(packed + 52, owner, 20);
    /* nonce as 8-byte big endian */
    for (int i = 0; i < 8; i++) {
        packed[79 - i] = (uint8_t) ((nonce >> (8 * i)) & 0xff);
    }
    memcpy(packed + 80, salt, 16);

    uint8_t expected_key[32];
    av_keccak256(packed, 96, expected_key);

    ASSERT_EQ(0, memcmp(predicted_key, expected_key, 32));
    PASS();
}

SUITE(abi)
{
    RUN_TEST(test_revert_catalog_decoding);
    RUN_TEST(test_abi_create_op);
    RUN_TEST(test_abi_patch_op);
    RUN_TEST(test_abi_extend_expiry_op);
    RUN_TEST(test_abi_delete_op);
    RUN_TEST(test_abi_roundtrip_batch);
    RUN_TEST(test_abi_predict_key);
}
