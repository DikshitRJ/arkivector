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

/* Backend constructors */
av_status av_signer_create_env(const char *env_var, av_signer **out, av_err *e);
av_status av_signer_create_keystore(const char *path, const char *password, av_signer **out, av_err *e);
av_status av_signer_create_remote(const char *endpoint, av_signer **out, av_err *e);
av_status av_signer_create(const char *type, const char *cfg, av_signer **out, av_err *e);

/* Query signer address (20 bytes) and hex representation (0x + 40 hex + NUL) */
const uint8_t *av_signer_get_address(const av_signer *s);
void av_signer_get_address_hex(const av_signer *s, char out_hex[43]);

/* Sign 32-byte hash, producing 32-byte r, 32-byte s, and 1-byte y-parity (v = 0 or 1) */
av_status av_signer_sign(const av_signer *s, const uint8_t hash32[32], uint8_t r[32],
                         uint8_t s_sig[32], uint8_t *v_parity, av_err *e);

/* Destroy signer and wipe sensitive key material with explicit_bzero */
void av_signer_destroy(av_signer *s);

#endif /* AV_ARKIV_SIGNER_H */
