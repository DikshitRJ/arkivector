/*
 * arkiv/signer.c: Cryptographic signer (env, keystore, remote) with secp256k1
 * and EIP-55 address formatting. Key material wiped with explicit_bzero.
 */
#include "arkiv/signer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>

#include "secp256k1.h"
#include "secp256k1_recovery.h"

#include "arkiv/entity.h"
#include "arkiv/keccak.h"
#include "arkiv/rpc.h"
#include "core/fileio.h"
#include "codec/hex.h"
#include "yyjson.h"

typedef enum { SIGNER_BACKEND_ENV, SIGNER_BACKEND_KEYSTORE, SIGNER_BACKEND_REMOTE } signer_backend;

struct av_signer {
    signer_backend backend;
    uint8_t address[20];
    secp256k1_context *secp;
    uint8_t privkey[32];
    char *remote_url;
    av_rpc *remote_rpc;     /* JSON-RPC client for the remote signer (owned) */
    char remote_method[32]; /* eth_signTransaction | account_signTransaction */
};

static bool hex_val(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') {
        *out = (uint8_t) (c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t) (c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t) (c - 'A' + 10);
        return true;
    }
    return false;
}

static bool parse_hex_key(const char *hex_str, uint8_t out[32])
{
    if (hex_str == NULL)
        return false;
    if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X'))
        hex_str += 2;
    if (strlen(hex_str) != 64)
        return false;
    for (int i = 0; i < 32; i++) {
        uint8_t hi, lo;
        if (!hex_val(hex_str[i * 2], &hi) || !hex_val(hex_str[i * 2 + 1], &lo))
            return false;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

/* Decode exactly n bytes of hex (optional 0x prefix, no other characters). */
static bool hex_fixed(const char *hex_str, uint8_t *out, size_t n)
{
    if (hex_str == NULL)
        return false;
    if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X'))
        hex_str += 2;
    if (strlen(hex_str) != n * 2)
        return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t hi, lo;
        if (!hex_val(hex_str[i * 2], &hi) || !hex_val(hex_str[i * 2 + 1], &lo))
            return false;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

static bool derive_address_from_privkey(secp256k1_context *ctx, const uint8_t priv[32], uint8_t addr[20])
{
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(ctx, &pub, priv))
        return false;

    uint8_t pub_serialized[65];
    size_t pub_len = sizeof(pub_serialized);
    if (!secp256k1_ec_pubkey_serialize(ctx, pub_serialized, &pub_len, &pub, SECP256K1_EC_UNCOMPRESSED))
        return false;

    uint8_t hash[32];
    av_keccak256(pub_serialized + 1, 64, hash);
    memcpy(addr, hash + 12, 20);
    return true;
}

av_status av_signer_create_env(const char *env_var, av_signer **out, av_err *e)
{
    *out = NULL;
    if (env_var == NULL) {
        av_err_set(e, "invalid_argument", "env_var name required");
        return AV_ERR;
    }

    const char *val = getenv(env_var);
    if (val == NULL || val[0] == '\0') {
        av_err_set(e, "invalid_argument", "environment variable '%s' not set or empty", env_var);
        return AV_ERR;
    }

    uint8_t priv[32];
    if (!parse_hex_key(val, priv)) {
        explicit_bzero(priv, sizeof(priv));
        av_err_set(e, "invalid_argument", "invalid hex private key in '%s'", env_var);
        return AV_ERR;
    }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (ctx == NULL) {
        explicit_bzero(priv, sizeof(priv));
        av_err_set(e, "internal", "failed to create secp256k1 context");
        return AV_ERR;
    }

    uint8_t addr[20];
    if (!derive_address_from_privkey(ctx, priv, addr)) {
        explicit_bzero(priv, sizeof(priv));
        secp256k1_context_destroy(ctx);
        av_err_set(e, "invalid_argument", "invalid secp256k1 private key");
        return AV_ERR;
    }

    av_signer *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        explicit_bzero(priv, sizeof(priv));
        secp256k1_context_destroy(ctx);
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    s->backend = SIGNER_BACKEND_ENV;
    s->secp = ctx;
    memcpy(s->privkey, priv, 32);
    memcpy(s->address, addr, 20);
    explicit_bzero(priv, sizeof(priv));

    *out = s;
    return AV_OK;
}

av_status av_signer_create_keystore(const char *path, const char *password, av_signer **out, av_err *e)
{
    *out = NULL;
    if (path == NULL || password == NULL) {
        av_err_set(e, "invalid_argument", "path and password required for keystore");
        return AV_ERR;
    }

    size_t rd = 0;
    char *buf = av_read_file(path, "keystore", (size_t) 1024 * 1024, &rd, e);
    if (buf == NULL) {
        return AV_ERR;
    }

    yyjson_doc *doc = yyjson_read(buf, rd, 0);
    free(buf);
    if (doc == NULL) {
        av_err_set(e, "invalid_argument", "keystore is not valid JSON");
        return AV_ERR;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *crypto = yyjson_obj_get(root, "crypto");
    if (crypto == NULL)
        crypto = yyjson_obj_get(root, "Crypto");
    if (crypto == NULL) {
        yyjson_doc_free(doc);
        av_err_set(e, "invalid_argument", "keystore missing crypto section");
        return AV_ERR;
    }

    const char *cipher = yyjson_get_str(yyjson_obj_get(crypto, "cipher"));
    const char *ciphertext_hex = yyjson_get_str(yyjson_obj_get(crypto, "ciphertext"));
    const char *kdf = yyjson_get_str(yyjson_obj_get(crypto, "kdf"));
    yyjson_val *kdfparams = yyjson_obj_get(crypto, "kdfparams");
    yyjson_val *cipherparams = yyjson_obj_get(crypto, "cipherparams");
    const char *iv_hex = cipherparams != NULL ? yyjson_get_str(yyjson_obj_get(cipherparams, "iv")) : NULL;
    const char *mac_hex = yyjson_get_str(yyjson_obj_get(crypto, "mac"));

    if (cipher == NULL || ciphertext_hex == NULL || kdf == NULL || iv_hex == NULL || mac_hex == NULL) {
        yyjson_doc_free(doc);
        av_err_set(e, "invalid_argument", "keystore missing required crypto fields");
        return AV_ERR;
    }

    /* Web3 Secret Storage v3: AES-128-CTR over a 32-byte private key, MAC = keccak256(dk[16..31] || ct). */
    uint8_t ct[32];
    uint8_t iv[16];
    uint8_t mac[32];
    uint8_t salt[64];
    const char *salt_hex = yyjson_get_str(yyjson_obj_get(kdfparams, "salt"));
    size_t salt_len = salt_hex != NULL ? strlen(salt_hex) / 2 : 0;
    if (strcmp(cipher, "aes-128-ctr") != 0 || !hex_fixed(ciphertext_hex, ct, sizeof(ct)) ||
        !hex_fixed(iv_hex, iv, sizeof(iv)) || !hex_fixed(mac_hex, mac, sizeof(mac)) || salt_len == 0 ||
        salt_len > sizeof(salt) || !hex_fixed(salt_hex, salt, salt_len)) {
        yyjson_doc_free(doc);
        av_err_set(e, "invalid_argument", "keystore: unsupported cipher or malformed ciphertext/iv/mac/salt");
        return AV_ERR;
    }

    /* Key derivation: 32-byte derived key. Parameters are bounded so a hostile file cannot exhaust memory/CPU. */
    uint8_t derived[32];
    memset(derived, 0, sizeof(derived));
    bool kdf_ok = false;
    if (strcmp(kdf, "scrypt") == 0) {
        uint64_t n_scrypt = yyjson_get_uint(yyjson_obj_get(kdfparams, "n"));
        uint64_t r_scrypt = yyjson_get_uint(yyjson_obj_get(kdfparams, "r"));
        uint64_t p_scrypt = yyjson_get_uint(yyjson_obj_get(kdfparams, "p"));
        uint64_t dklen = yyjson_get_uint(yyjson_obj_get(kdfparams, "dklen"));
        bool pow2 = n_scrypt >= 2 && (n_scrypt & (n_scrypt - 1)) == 0;
        if (pow2 && n_scrypt <= ((uint64_t) 1 << 20) && r_scrypt >= 1 && r_scrypt <= 16 && p_scrypt >= 1 &&
            p_scrypt <= 16 && dklen == 32) {
            /* geth "standard" (n=2^18, r=8) needs 128*r*n = 256 MiB: OpenSSL's default cap is 32 MiB. */
            uint64_t maxmem = 128 * r_scrypt * (n_scrypt + p_scrypt + 2) + ((uint64_t) 1 << 20);
            kdf_ok = EVP_PBE_scrypt(password, strlen(password), salt, salt_len, n_scrypt, r_scrypt, p_scrypt,
                                    maxmem, derived, sizeof(derived)) == 1;
        }
    } else if (strcmp(kdf, "pbkdf2") == 0) {
        int64_t c_pbkdf = yyjson_get_sint(yyjson_obj_get(kdfparams, "c"));
        uint64_t dklen = yyjson_get_uint(yyjson_obj_get(kdfparams, "dklen"));
        const char *prf = yyjson_get_str(yyjson_obj_get(kdfparams, "prf"));
        if (c_pbkdf >= 1 && c_pbkdf <= 10000000 && dklen == 32 && prf != NULL &&
            strcmp(prf, "hmac-sha256") == 0) {
            kdf_ok = PKCS5_PBKDF2_HMAC(password, (int) strlen(password), salt, (int) salt_len, (int) c_pbkdf,
                                       EVP_sha256(), (int) sizeof(derived), derived) == 1;
        }
    }
    yyjson_doc_free(doc);

    if (!kdf_ok) {
        explicit_bzero(derived, sizeof(derived));
        av_err_set(e, "invalid_argument", "keystore key derivation failed (unsupported kdf or parameters)");
        return AV_ERR;
    }

    uint8_t mac_payload[16 + sizeof(ct)];
    memcpy(mac_payload, derived + 16, 16);
    memcpy(mac_payload + 16, ct, sizeof(ct));
    uint8_t computed_mac[32];
    av_keccak256(mac_payload, sizeof(mac_payload), computed_mac);
    explicit_bzero(mac_payload, sizeof(mac_payload));

    if (CRYPTO_memcmp(computed_mac, mac, sizeof(mac)) != 0) {
        explicit_bzero(derived, sizeof(derived));
        av_err_set(e, "invalid_argument", "keystore password incorrect (MAC mismatch)");
        return AV_ERR;
    }

    uint8_t priv[32];
    int out_len1 = 0;
    int out_len2 = 0;
    EVP_CIPHER_CTX *cctx = EVP_CIPHER_CTX_new();
    bool dec_ok = cctx != NULL && EVP_DecryptInit_ex(cctx, EVP_aes_128_ctr(), NULL, derived, iv) == 1 &&
                  EVP_DecryptUpdate(cctx, priv, &out_len1, ct, (int) sizeof(ct)) == 1 && out_len1 >= 0 &&
                  out_len1 <= (int) sizeof(priv) &&
                  EVP_DecryptFinal_ex(cctx, priv + out_len1, &out_len2) == 1 &&
                  out_len1 + out_len2 == (int) sizeof(priv);
    EVP_CIPHER_CTX_free(cctx);
    explicit_bzero(derived, sizeof(derived));
    if (!dec_ok) {
        explicit_bzero(priv, sizeof(priv));
        av_err_set(e, "internal", "keystore decryption failed");
        return AV_ERR;
    }

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t addr[20];
    if (ctx == NULL || !derive_address_from_privkey(ctx, priv, addr)) {
        explicit_bzero(priv, sizeof(priv));
        secp256k1_context_destroy(ctx);
        av_err_set(e, "invalid_argument", "decrypted key is not a valid secp256k1 key");
        return AV_ERR;
    }

    av_signer *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        explicit_bzero(priv, sizeof(priv));
        secp256k1_context_destroy(ctx);
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->backend = SIGNER_BACKEND_KEYSTORE;
    s->secp = ctx;
    memcpy(s->privkey, priv, 32);
    memcpy(s->address, addr, 20);
    explicit_bzero(priv, sizeof(priv));

    *out = s;
    return AV_OK;
}

av_status av_signer_create_remote(const char *endpoint, av_signer **out, av_err *e)
{
    av_signer_remote_config rc;
    memset(&rc, 0, sizeof(rc));
    rc.url = endpoint;
    return av_signer_create_remote_ex(&rc, out, e);
}

/* Ask the signer for its accounts; exactly one is required when no address is configured. */
static av_status remote_resolve_address(av_signer *s, av_err *e)
{
    const char *method =
        strcmp(s->remote_method, "account_signTransaction") == 0 ? "account_list" : "eth_accounts";
    yyjson_doc *doc = NULL;
    yyjson_val *res = NULL;
    if (av_signer_remote_call(s, method, "[]", &doc, &res, e) != AV_OK)
        return AV_ERR;
    bool ok = yyjson_is_arr(res) && yyjson_arr_size(res) == 1 &&
              av_addr_from_hex(yyjson_get_str(yyjson_arr_get_first(res)), s->address);
    size_t n = yyjson_is_arr(res) ? yyjson_arr_size(res) : 0;
    yyjson_doc_free(doc);
    if (!ok) {
        av_err_set(e, "invalid_argument",
                   "remote signer: %s returned %zu accounts; set write.signer.remote_address to choose one",
                   method, n);
        return AV_ERR;
    }
    return AV_OK;
}

av_status av_signer_create_remote_ex(const av_signer_remote_config *cfg, av_signer **out, av_err *e)
{
    *out = NULL;
    if (cfg == NULL || cfg->url == NULL || cfg->url[0] == '\0') {
        av_err_set(e, "invalid_argument", "remote signer: url required");
        return AV_ERR;
    }
    if (strncmp(cfg->url, "http://", 7) != 0 && strncmp(cfg->url, "https://", 8) != 0) {
        av_err_set(e, "invalid_argument", "remote signer: url must start with http:// or https://");
        return AV_ERR;
    }
    const char *method = cfg->method != NULL && cfg->method[0] != '\0' ? cfg->method : "eth_signTransaction";
    if (strcmp(method, "eth_signTransaction") != 0 && strcmp(method, "account_signTransaction") != 0) {
        av_err_set(e, "invalid_argument",
                   "remote signer: method must be eth_signTransaction (Web3Signer, geth) or "
                   "account_signTransaction (Clef)");
        return AV_ERR;
    }

    av_signer *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->backend = SIGNER_BACKEND_REMOTE;
    s->remote_url = strdup(cfg->url);
    snprintf(s->remote_method, sizeof(s->remote_method), "%s", method);
    const char *urls[1] = {cfg->url};
    av_rpc_config rc;
    memset(&rc, 0, sizeof(rc));
    rc.urls = urls;
    rc.n_urls = 1;
    rc.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 10000;
    rc.max_retries = 2; /* signing the same transaction twice yields the same nonce: safe to retry */
    rc.chain_id = cfg->chain_id;
    s->remote_rpc = s->remote_url != NULL ? av_rpc_create(&rc, e) : NULL;
    if (s->remote_rpc == NULL) {
        if (s->remote_url == NULL)
            av_err_set(e, "internal", "out of memory");
        av_signer_destroy(s);
        return AV_ERR;
    }
    if (cfg->address != NULL && cfg->address[0] != '\0') {
        if (!av_addr_from_hex(cfg->address, s->address)) {
            av_signer_destroy(s);
            av_err_set(e, "invalid_argument", "remote signer: remote_address is not a 20-byte hex address");
            return AV_ERR;
        }
    } else if (remote_resolve_address(s, e) != AV_OK) {
        av_signer_destroy(s);
        return AV_ERR;
    }
    *out = s;
    return AV_OK;
}

av_signer_kind av_signer_get_kind(const av_signer *s)
{
    if (s == NULL || s->backend == SIGNER_BACKEND_ENV)
        return AV_SIGNER_ENV;
    return s->backend == SIGNER_BACKEND_KEYSTORE ? AV_SIGNER_KEYSTORE : AV_SIGNER_REMOTE;
}

const char *av_signer_kind_name(const av_signer *s)
{
    switch (av_signer_get_kind(s)) {
    case AV_SIGNER_KEYSTORE:
        return "keystore";
    case AV_SIGNER_REMOTE:
        return "remote";
    case AV_SIGNER_ENV:
    default:
        return "env";
    }
}

const char *av_signer_remote_method(const av_signer *s)
{
    return (s != NULL && s->backend == SIGNER_BACKEND_REMOTE) ? s->remote_method : NULL;
}

av_status av_signer_remote_call(const av_signer *s, const char *method, const char *params_json,
                                yyjson_doc **doc, yyjson_val **result, av_err *e)
{
    *doc = NULL;
    *result = NULL;
    if (s == NULL || s->backend != SIGNER_BACKEND_REMOTE || s->remote_rpc == NULL) {
        av_err_set(e, "invalid_argument", "not a remote signer");
        return AV_ERR;
    }
    int code = 0;
    if (av_rpc_call(s->remote_rpc, method, params_json, doc, result, &code, e) != AV_OK) {
        char msg[sizeof(e->msg)];
        snprintf(msg, sizeof(msg), "%s", e->msg);
        av_err_set(e, "write_disabled", "remote signer %s failed: %s", method, msg);
        return AV_ERR;
    }
    return AV_OK;
}

av_status av_signer_create(const char *type, const char *cfg, av_signer **out, av_err *e)
{
    if (type == NULL) {
        av_err_set(e, "invalid_argument", "signer type required");
        return AV_ERR;
    }
    if (strcmp(type, "env") == 0) {
        return av_signer_create_env(cfg != NULL ? cfg : "ARKIV_PRIVATE_KEY", out, e);
    }
    if (strcmp(type, "keystore") == 0) {
        return av_signer_create_keystore(cfg, "", out, e);
    }
    if (strcmp(type, "remote") == 0) {
        return av_signer_create_remote(cfg, out, e);
    }
    av_err_set(e, "invalid_argument", "unknown signer type: %s", type);
    return AV_ERR;
}

const uint8_t *av_signer_get_address(const av_signer *s)
{
    return s != NULL ? s->address : NULL;
}

void av_signer_get_address_hex(const av_signer *s, char out_hex[43])
{
    if (s == NULL) {
        if (out_hex != NULL)
            out_hex[0] = '\0';
        return;
    }
    const uint8_t *addr = s->address;
    static const char hex_chars[] = "0123456789abcdef";
    char raw[41];
    for (int i = 0; i < 20; i++) {
        raw[i * 2] = hex_chars[(addr[i] >> 4) & 0x0f];
        raw[i * 2 + 1] = hex_chars[addr[i] & 0x0f];
    }
    raw[40] = '\0';

    uint8_t hash[32];
    av_keccak256((const uint8_t *) raw, 40, hash);

    out_hex[0] = '0';
    out_hex[1] = 'x';
    for (int i = 0; i < 40; i++) {
        uint8_t hash_byte = hash[i / 2];
        uint8_t nibble = (i % 2 == 0) ? (hash_byte >> 4) : (hash_byte & 0x0f);
        if (raw[i] >= 'a' && raw[i] <= 'f' && nibble >= 8) {
            out_hex[2 + i] = (char) (raw[i] - 'a' + 'A');
        } else {
            out_hex[2 + i] = raw[i];
        }
    }
    out_hex[42] = '\0';
}

av_status av_signer_sign(const av_signer *s, const uint8_t hash32[32], uint8_t r[32], uint8_t s_sig[32],
                         uint8_t *v_parity, av_err *e)
{
    if (s == NULL || hash32 == NULL || r == NULL || s_sig == NULL || v_parity == NULL) {
        av_err_set(e, "invalid_argument", "invalid arguments to av_signer_sign");
        return AV_ERR;
    }

    if (s->backend == SIGNER_BACKEND_REMOTE) {
        av_err_set(e, "internal", "a remote signer signs whole transactions, not hashes");
        return AV_ERR;
    }

    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(s->secp, &sig, hash32, s->privkey, NULL, NULL)) {
        av_err_set(e, "internal", "secp256k1 signing failed");
        return AV_ERR;
    }

    uint8_t compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(s->secp, compact, &recid, &sig);

    memcpy(r, compact, 32);
    memcpy(s_sig, compact + 32, 32);
    *v_parity = (uint8_t) recid;

    return AV_OK;
}

void av_signer_destroy(av_signer *s)
{
    if (s == NULL)
        return;
    explicit_bzero(s->privkey, sizeof(s->privkey));
    if (s->secp != NULL) {
        secp256k1_context_destroy(s->secp);
        s->secp = NULL;
    }
    if (s->remote_rpc != NULL)
        av_rpc_destroy(s->remote_rpc);
    free(s->remote_url);
    free(s);
}
