/*
 * arkiv/signer.h: cryptographic signer interface supporting env, keystore, and remote backends.
 */
#ifndef AV_ARKIV_SIGNER_H
#define AV_ARKIV_SIGNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

typedef struct av_signer av_signer;

typedef enum { AV_SIGNER_ENV = 0, AV_SIGNER_KEYSTORE = 1, AV_SIGNER_REMOTE = 2 } av_signer_kind;

/*
 * Remote signer (GitHub issue #12): a Web3Signer / geth-style JSON-RPC endpoint that signs whole
 * transactions (eth_signTransaction), or Clef (account_signTransaction). The daemon never holds the
 * key. Every signed transaction it returns is decoded and checked against the request (arkiv/tx.c).
 */
typedef struct {
    const char *url; /* http(s) JSON-RPC endpoint (required) */
    const char
        *address; /* signing address "0x..." ; NULL/"" = ask eth_accounts / account_list (must be one) */
    const char *method; /* "eth_signTransaction" (default) or "account_signTransaction" (Clef) */
    uint64_t chain_id;
    int timeout_ms; /* per call; <= 0 = 10000 */
} av_signer_remote_config;

/* Backend constructors */
av_status av_signer_create_env(const char *env_var, av_signer **out, av_err *e);
av_status av_signer_create_keystore(const char *path, const char *password, av_signer **out, av_err *e);
av_status av_signer_create_remote(const char *endpoint, av_signer **out, av_err *e);
av_status av_signer_create_remote_ex(const av_signer_remote_config *cfg, av_signer **out, av_err *e);
av_status av_signer_create(const char *type, const char *cfg, av_signer **out, av_err *e);

/* Backend kind and its name ("env", "keystore", "remote"). */
av_signer_kind av_signer_get_kind(const av_signer *s);
const char *av_signer_kind_name(const av_signer *s);

/*
 * Remote backend only: one JSON-RPC call to the signer. method is the configured signing method
 * (av_signer_remote_method). On success *doc owns the response (yyjson_doc_free) and *result points
 * into it. AV_ERR with "invalid_argument" for a non-remote signer.
 */
struct yyjson_doc;
struct yyjson_val;
const char *av_signer_remote_method(const av_signer *s);
av_status av_signer_remote_call(const av_signer *s, const char *method, const char *params_json,
                                struct yyjson_doc **doc, struct yyjson_val **result, av_err *e);

/* Query signer address (20 bytes) and hex representation (0x + 40 hex + NUL) */
const uint8_t *av_signer_get_address(const av_signer *s);
void av_signer_get_address_hex(const av_signer *s, char out_hex[43]);

/* Sign 32-byte hash, producing 32-byte r, 32-byte s, and 1-byte y-parity (v = 0 or 1). Local backends
 * (env, keystore) only: a remote signer signs whole transactions (av_tx_sign_and_encode). */
av_status av_signer_sign(const av_signer *s, const uint8_t hash32[32], uint8_t r[32], uint8_t s_sig[32],
                         uint8_t *v_parity, av_err *e);

/* Destroy signer and wipe sensitive key material with explicit_bzero */
void av_signer_destroy(av_signer *s);

#endif /* AV_ARKIV_SIGNER_H */
