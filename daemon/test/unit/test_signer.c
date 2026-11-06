/*
 * test_signer.c: unit tests for environment signer, address derivation (EIP-55),
 * and secp256k1 (r, s, v) signing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/signer.h"
#include "greatest.h"

TEST test_env_signer_create_hardhat_key(void)
{
    /* Hardhat account 0 test vector:
     * priv: ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
     * addr: 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
     */
    static const uint8_t expected_addr[20] = {
        0xf3, 0x9f, 0xd6, 0xe5, 0x1a, 0xad, 0x88, 0xf6, 0xf4, 0xce,
        0x6a, 0xb8, 0x82, 0x72, 0x79, 0xcf, 0xff, 0xb9, 0x22, 0x66
    };

    setenv("ARKIV_TEST_SIGNER_KEY_1", "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80", 1);

    av_signer *s = NULL;
    av_err err;
    av_err_clear(&err);

    av_status st = av_signer_create_env("ARKIV_TEST_SIGNER_KEY_1", &s, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(s != NULL);

    const uint8_t *addr = av_signer_get_address(s);
    ASSERT(addr != NULL);
    ASSERT_EQ(0, memcmp(expected_addr, addr, 20));

    char addr_hex[43];
    memset(addr_hex, 0, sizeof(addr_hex));
    av_signer_get_address_hex(s, addr_hex);
    ASSERT_STR_EQ("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266", addr_hex);

    av_signer_destroy(s);
    PASS();
}

TEST test_env_signer_create_with_0x_prefix(void)
{
    /* Private key 1 test vector:
     * priv: 0x0000000000000000000000000000000000000000000000000000000000000001
     * addr: 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf
     */
    static const uint8_t expected_addr[20] = {
        0x7e, 0x5f, 0x45, 0x52, 0x09, 0x1a, 0x69, 0x12, 0x5d, 0x5d,
        0xfc, 0xb7, 0xb8, 0xc2, 0x65, 0x90, 0x29, 0x39, 0x5b, 0xdf
    };

    setenv("ARKIV_TEST_SIGNER_KEY_2", "0x0000000000000000000000000000000000000000000000000000000000000001", 1);

    av_signer *s = NULL;
    av_err err;
    av_err_clear(&err);

    av_status st = av_signer_create_env("ARKIV_TEST_SIGNER_KEY_2", &s, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(s != NULL);

    const uint8_t *addr = av_signer_get_address(s);
    ASSERT(addr != NULL);
    ASSERT_EQ(0, memcmp(expected_addr, addr, 20));

    char addr_hex[43];
    memset(addr_hex, 0, sizeof(addr_hex));
    av_signer_get_address_hex(s, addr_hex);
    ASSERT_STR_EQ("0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf", addr_hex);

    av_signer_destroy(s);
    PASS();
}

TEST test_env_signer_sign_hash(void)
{
    setenv("ARKIV_TEST_SIGNER_KEY_3", "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80", 1);

    av_signer *s = NULL;
    av_err err;
    av_err_clear(&err);

    av_status st = av_signer_create_env("ARKIV_TEST_SIGNER_KEY_3", &s, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(s != NULL);

    uint8_t hash32[32];
    for (int i = 0; i < 32; i++) {
        hash32[i] = (uint8_t) (i * 3 + 7);
    }

    uint8_t r1[32] = {0};
    uint8_t s1[32] = {0};
    uint8_t v1 = 255;

    st = av_signer_sign(s, hash32, r1, s1, &v1, &err);
    ASSERT_EQ(AV_OK, st);

    /* Verify r is non-zero */
    uint8_t zero[32] = {0};
    ASSERT(memcmp(r1, zero, 32) != 0);

    /* Verify s is non-zero */
    ASSERT(memcmp(s1, zero, 32) != 0);

    /* Verify v is 0 or 1 */
    ASSERT(v1 == 0 || v1 == 1);

    /* Verify canonical Low-S (s <= N/2), so top bit of s[0] must be 0 */
    ASSERT((s1[0] & 0x80) == 0);

    /* Determinism check: signing identical hash produces identical signature */
    uint8_t r2[32] = {0};
    uint8_t s2[32] = {0};
    uint8_t v2 = 255;
    st = av_signer_sign(s, hash32, r2, s2, &v2, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(0, memcmp(r1, r2, 32));
    ASSERT_EQ(0, memcmp(s1, s2, 32));
    ASSERT_EQ(v1, v2);

    av_signer_destroy(s);
    PASS();
}

TEST test_env_signer_invalid_inputs(void)
{
    av_signer *s = NULL;
    av_err err;
    av_err_clear(&err);

    /* Missing env variable */
    unsetenv("NON_EXISTENT_VAR_ARKIV_12345");
    av_status st = av_signer_create_env("NON_EXISTENT_VAR_ARKIV_12345", &s, &err);
    ASSERT_EQ(AV_ERR, st);
    ASSERT(s == NULL);

    /* Invalid hex: too short */
    setenv("ARKIV_SHORT_KEY", "1234abcd", 1);
    st = av_signer_create_env("ARKIV_SHORT_KEY", &s, &err);
    ASSERT_EQ(AV_ERR, st);
    ASSERT(s == NULL);

    /* Invalid hex: invalid character */
    setenv("ARKIV_BAD_HEX_KEY", "zz0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80", 1);
    st = av_signer_create_env("ARKIV_BAD_HEX_KEY", &s, &err);
    ASSERT_EQ(AV_ERR, st);
    ASSERT(s == NULL);

    PASS();
}

SUITE(signer)
{
    RUN_TEST(test_env_signer_create_hardhat_key);
    RUN_TEST(test_env_signer_create_with_0x_prefix);
    RUN_TEST(test_env_signer_sign_hash);
    RUN_TEST(test_env_signer_invalid_inputs);
}
