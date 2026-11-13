/*
 * test_rawtx.c: signed EIP-1559 transaction decoder (arkiv/rawtx.c), av_tx_matches, and the
 * Web3 Secret Storage v3 keystore signer (GitHub issue #12).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arkiv/rawtx.h"
#include "arkiv/signer.h"
#include "arkiv/tx.h"
#include "core/arena.h"
#include "greatest.h"

static const uint8_t k_contract[20] = {0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x44};
static const uint8_t k_data[8] = {0x49, 0x65, 0x00, 0x44, 0xde, 0xad, 0xbe, 0xef};

static av_signer *hardhat_signer(void)
{
    setenv("ARKIV_TEST_RAWTX_KEY", "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80", 1);
    av_signer *s = NULL;
    av_err e;
    av_err_clear(&e);
    if (av_signer_create_env("ARKIV_TEST_RAWTX_KEY", &s, &e) != AV_OK)
        return NULL;
    return s;
}

static void fill_desc(av_tx_desc *d)
{
    memset(d, 0, sizeof(*d));
    d->chain_id = 7794095;
    d->nonce = 12;
    d->max_priority_fee_per_gas = 1500000000ULL;
    d->max_fee_per_gas = 20000000000ULL;
    d->gas_limit = 412000;
    memcpy(d->to, k_contract, 20);
    d->data = k_data;
    d->data_len = sizeof(k_data);
}

TEST test_rawtx_roundtrip(void)
{
    av_signer *s = hardhat_signer();
    ASSERT(s != NULL);
    av_arena *a = av_arena_create("t", 4096, 0);
    av_tx_desc d;
    fill_desc(&d);
    uint8_t *raw = NULL;
    size_t len = 0;
    uint8_t hash[32];
    av_err e;
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_tx_sign_and_encode(&d, s, a, &raw, &len, hash, &e));

    av_rawtx rt;
    ASSERT_EQ(AV_OK, av_rawtx_decode(raw, len, &rt, &e));
    ASSERT_EQ(d.chain_id, rt.chain_id);
    ASSERT_EQ(d.nonce, rt.nonce);
    ASSERT_EQ(d.max_priority_fee_per_gas, rt.max_priority_fee_per_gas);
    ASSERT_EQ(d.max_fee_per_gas, rt.max_fee_per_gas);
    ASSERT_EQ(d.gas_limit, rt.gas_limit);
    ASSERT(rt.has_to && rt.value_zero);
    ASSERT_EQ(0, memcmp(rt.to, k_contract, 20));
    ASSERT_EQ(sizeof(k_data), rt.data_len);
    ASSERT_EQ(0, memcmp(rt.data, k_data, sizeof(k_data)));
    ASSERT_EQ(0u, rt.n_access_list);
    ASSERT_EQ(0, memcmp(rt.sender, av_signer_get_address(s), 20));
    ASSERT_EQ(0, memcmp(rt.hash, hash, 32));
    ASSERT(av_tx_matches(&d, &rt));

    av_tx_desc d2 = d;
    d2.gas_limit++;
    ASSERT_FALSE(av_tx_matches(&d2, &rt));
    d2 = d;
    d2.nonce = 13;
    ASSERT_FALSE(av_tx_matches(&d2, &rt));
    d2 = d;
    d2.to[5] = 1;
    ASSERT_FALSE(av_tx_matches(&d2, &rt));

    /* Hex entry point, with and without 0x. */
    static const char hexd[] = "0123456789abcdef";
    char *hex = malloc(len * 2 + 3);
    ASSERT(hex != NULL);
    hex[0] = '0';
    hex[1] = 'x';
    for (size_t i = 0; i < len; i++) {
        hex[2 + 2 * i] = hexd[raw[i] >> 4];
        hex[3 + 2 * i] = hexd[raw[i] & 15];
    }
    hex[2 + 2 * len] = '\0';
    uint8_t *raw2 = NULL;
    size_t len2 = 0;
    av_rawtx rt2;
    ASSERT_EQ(AV_OK, av_rawtx_decode_hex(hex, strlen(hex), a, &raw2, &len2, &rt2, &e));
    ASSERT_EQ(len, len2);
    ASSERT_EQ(0, memcmp(rt2.sender, rt.sender, 20));
    ASSERT_EQ(AV_ERR, av_rawtx_decode_hex("0xzz", 4, a, &raw2, &len2, &rt2, &e));
    free(hex);

    av_arena_destroy(a);
    av_signer_destroy(s);
    PASS();
}

TEST test_rawtx_rejects_malformed(void)
{
    av_signer *s = hardhat_signer();
    ASSERT(s != NULL);
    av_arena *a = av_arena_create("t", 4096, 0);
    av_tx_desc d;
    fill_desc(&d);
    uint8_t *raw = NULL;
    size_t len = 0;
    av_err e;
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_tx_sign_and_encode(&d, s, a, &raw, &len, NULL, &e));
    av_rawtx rt;

    /* Every truncation fails. */
    for (size_t n = 0; n < len; n++)
        ASSERT_EQ(AV_ERR, av_rawtx_decode(raw, n, &rt, &e));

    /* Trailing byte. */
    uint8_t *buf = malloc(len + 1);
    ASSERT(buf != NULL);
    memcpy(buf, raw, len);
    buf[len] = 0;
    ASSERT_EQ(AV_ERR, av_rawtx_decode(buf, len + 1, &rt, &e));

    /* Other transaction types. */
    memcpy(buf, raw, len);
    buf[0] = 0x01;
    ASSERT_EQ(AV_ERR, av_rawtx_decode(buf, len, &rt, &e));
    static const uint8_t legacy[] = {0xc3, 0x80, 0x80, 0x80};
    ASSERT_EQ(AV_ERR, av_rawtx_decode(legacy, sizeof(legacy), &rt, &e));
    ASSERT(strstr(e.msg, "legacy") != NULL);
    static const uint8_t empty_list[] = {0x02, 0xc0};
    ASSERT_EQ(AV_ERR, av_rawtx_decode(empty_list, sizeof(empty_list), &rt, &e));

    /* A flipped data byte changes the sighash: the sender changes (or recovery fails). */
    memcpy(buf, raw, len);
    for (size_t i = 0; i + 4 < len; i++) {
        if (memcmp(buf + i, k_data + 4, 4) == 0) {
            buf[i] ^= 0x01;
            break;
        }
    }
    av_status st = av_rawtx_decode(buf, len, &rt, &e);
    ASSERT(st == AV_ERR || memcmp(rt.sender, av_signer_get_address(s), 20) != 0);
    free(buf);

    /* High-s signature (EIP-2): rebuild with s' = n - s and flipped parity. */
    uint8_t sighash[32], r[32], sv[32], v = 0;
    ASSERT_EQ(AV_OK, av_tx_sighash(&d, sighash, &e));
    ASSERT_EQ(AV_OK, av_signer_sign(s, sighash, r, sv, &v, &e));
    static const uint8_t n_order[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48,
                                        0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};
    uint8_t hs[32];
    int borrow = 0;
    for (int i = 31; i >= 0; i--) {
        int dv = (int) n_order[i] - (int) sv[i] - borrow;
        borrow = dv < 0;
        hs[i] = (uint8_t) (dv + (borrow ? 256 : 0));
    }
    uint8_t *raw_hi = NULL;
    size_t len_hi = 0;
    ASSERT_EQ(AV_OK, av_tx_encode_signed(&d, (uint8_t) (v ^ 1), r, hs, a, &raw_hi, &len_hi, NULL, &e));
    ASSERT_EQ(AV_ERR, av_rawtx_decode(raw_hi, len_hi, &rt, &e));
    ASSERT(strstr(e.msg, "EIP-2") != NULL);

    av_arena_destroy(a);
    av_signer_destroy(s);
    PASS();
}

TEST test_rawtx_noncanonical_integer(void)
{
    /* 0x02 || [chainId=0x0001 (leading zero) ...]: rejected before signature checks. */
    static const uint8_t bad[] = {0x02, 0xce, 0x82, 0x00, 0x01, 0x80, 0x80, 0x80,
                                  0x80, 0x80, 0x80, 0x80, 0xc0, 0x80, 0x01, 0x01};
    av_rawtx rt;
    av_err e;
    av_err_clear(&e);
    ASSERT_EQ(AV_ERR, av_rawtx_decode(bad, sizeof(bad), &rt, &e));
    ASSERT(strstr(e.msg, "canonical") != NULL);
    /* A single byte < 0x80 wrapped as a string (0x81 0x05) is not canonical either. */
    static const uint8_t bad2[] = {0x02, 0xcd, 0x81, 0x05, 0x80, 0x80, 0x80, 0x80,
                                   0x80, 0x80, 0x80, 0xc0, 0x80, 0x01, 0x01};
    ASSERT_EQ(AV_ERR, av_rawtx_decode(bad2, sizeof(bad2), &rt, &e));
    ASSERT(strstr(e.msg, "malformed RLP") != NULL);
    PASS();
}

TEST test_keystore_v3_pbkdf2_vector(void)
{
    /* Web3 Secret Storage Definition test vector (pbkdf2-sha256, c = 262144). */
    static const char *ks =
        "{\"crypto\":{\"cipher\":\"aes-128-ctr\",\"cipherparams\":{\"iv\":"
        "\"6087dab2f9fdbbfaddc31a909735c1e6\"},"
        "\"ciphertext\":\"5318b4d5bcd28de64ee5559e671353e16f075ecae9f99c7a79a38af5f869aa46\",\"kdf\":"
        "\"pbkdf2\","
        "\"kdfparams\":{\"c\":262144,\"dklen\":32,\"prf\":\"hmac-sha256\",\"salt\":"
        "\"ae3cd4e7013836a3df6bd7241b12db061dbe2c6785853cce422d148a624ce0bd\"},\"mac\":"
        "\"517ead924a9d0dc3124507e3393d175ce3ff7c1e96529c6c555ce9e51205e9b2\"},\"id\":"
        "\"3198bc9c-6672-5ab3-d995-4942343ae5b6\",\"version\":3}";
    char path[] = "/tmp/av_ks_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    ASSERT_EQ((ssize_t) strlen(ks), write(fd, ks, strlen(ks)));
    close(fd);

    av_signer *s = NULL;
    av_err e;
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_signer_create_keystore(path, "testpassword", &s, &e));
    char hex[43];
    av_signer_get_address_hex(s, hex);
    ASSERT_STR_EQ("0x008AeEda4D805471dF9b2A5B0f38A0C3bCBA786b", hex);
    ASSERT_EQ(AV_SIGNER_KEYSTORE, av_signer_get_kind(s));
    ASSERT_STR_EQ("keystore", av_signer_kind_name(s));
    av_signer_destroy(s);

    s = NULL;
    ASSERT_EQ(AV_ERR, av_signer_create_keystore(path, "wrong", &s, &e));
    ASSERT(s == NULL);
    ASSERT(strstr(e.msg, "MAC mismatch") != NULL);
    unlink(path);
    PASS();
}

TEST test_remote_signer_config_errors(void)
{
    av_signer *s = NULL;
    av_err e;
    av_err_clear(&e);
    av_signer_remote_config rc;
    memset(&rc, 0, sizeof(rc));
    ASSERT_EQ(AV_ERR, av_signer_create_remote_ex(&rc, &s, &e));
    rc.url = "ftp://signer";
    ASSERT_EQ(AV_ERR, av_signer_create_remote_ex(&rc, &s, &e));
    rc.url = "http://127.0.0.1:9";
    rc.method = "eth_sign";
    ASSERT_EQ(AV_ERR, av_signer_create_remote_ex(&rc, &s, &e));
    rc.method = NULL;
    rc.address = "0x1234";
    ASSERT_EQ(AV_ERR, av_signer_create_remote_ex(&rc, &s, &e));
    /* A configured address needs no round trip: creation succeeds even if the signer is down. */
    rc.address = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
    ASSERT_EQ(AV_OK, av_signer_create_remote_ex(&rc, &s, &e));
    ASSERT_EQ(AV_SIGNER_REMOTE, av_signer_get_kind(s));
    ASSERT_STR_EQ("eth_signTransaction", av_signer_remote_method(s));
    uint8_t h[32] = {0}, r[32], sv[32], v;
    ASSERT_EQ(AV_ERR, av_signer_sign(s, h, r, sv, &v, &e)); /* hashes are never signed remotely */
    av_signer_destroy(s);
    PASS();
}

SUITE(rawtx)
{
    RUN_TEST(test_rawtx_roundtrip);
    RUN_TEST(test_rawtx_rejects_malformed);
    RUN_TEST(test_rawtx_noncanonical_integer);
    RUN_TEST(test_keystore_v3_pbkdf2_vector);
    RUN_TEST(test_remote_signer_config_errors);
}
