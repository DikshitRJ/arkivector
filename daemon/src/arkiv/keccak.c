/*
 * arkiv/keccak.c: Keccak-256 sponge over tiny_sha3's sha3_keccakf permutation.
 * See keccak.h.
 */
#include "arkiv/keccak.h"

#include <string.h>

#include "sha3.h"

#define AV_KECCAK_RATE 136 /* 200 - 2 * 32 */

void av_keccak256(const void *data, size_t len, uint8_t out[32])
{
    union {
        uint8_t b[200];
        uint64_t q[25];
    } st;
    const uint8_t *p = (const uint8_t *) data;
    size_t i;

    memset(&st, 0, sizeof st);
    while (len >= AV_KECCAK_RATE) {
        for (i = 0; i < AV_KECCAK_RATE; i++)
            st.b[i] ^= p[i];
        sha3_keccakf(st.q);
        p += AV_KECCAK_RATE;
        len -= AV_KECCAK_RATE;
    }
    for (i = 0; i < len; i++)
        st.b[i] ^= p[i];
    st.b[len] ^= 0x01; /* Keccak (pre-NIST) domain padding */
    st.b[AV_KECCAK_RATE - 1] ^= 0x80;
    sha3_keccakf(st.q);
    memcpy(out, st.b, 32);
}
