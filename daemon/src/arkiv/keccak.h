/*
 * arkiv/keccak.h: Keccak-256 as used by Ethereum (padding 0x01, not SHA3's 0x06).
 *
 * Built on the vendored tiny_sha3 permutation (sha3_keccakf); the vendored file is unmodified.
 * Thread-safe: no shared state.
 */
#ifndef AV_ARKIV_KECCAK_H
#define AV_ARKIV_KECCAK_H

#include <stddef.h>
#include <stdint.h>

/* out receives the 32-byte digest of data[0..len). data may be NULL only when len is 0. */
void av_keccak256(const void *data, size_t len, uint8_t out[32]);

#endif /* AV_ARKIV_KECCAK_H */
