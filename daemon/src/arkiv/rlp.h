/*
 * arkiv/rlp.h: Recursive Length Prefix encoding (Ethereum Yellow Paper, appendix B),
 * used for EIP-1559 transactions (arkiv/tx.c).
 *
 * Builder over a growable buffer: items are appended in order; lists are opened with
 * av_rlp_list_begin and closed with av_rlp_list_end (nested lists supported). The
 * list header length is fixed up on close. Not thread-safe (one builder per thread).
 */
#ifndef AV_ARKIV_RLP_H
#define AV_ARKIV_RLP_H

#include <stddef.h>
#include <stdint.h>

#include "log.h"

#define AV_RLP_MAX_DEPTH 16

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    size_t open[AV_RLP_MAX_DEPTH]; /* payload start offsets of open lists */
    int depth;
    int failed; /* allocation failure or misuse */
} av_rlp;

void av_rlp_init(av_rlp *r);
void av_rlp_free(av_rlp *r);

/* Byte string (canonical: a single byte < 0x80 is encoded as itself). */
void av_rlp_bytes(av_rlp *r, const uint8_t *data, size_t n);
/* Unsigned integer, big-endian with no leading zeros (0 encodes as the empty string). */
void av_rlp_u64(av_rlp *r, uint64_t v);
/* 256-bit big-endian unsigned integer (leading zeros stripped). */
void av_rlp_u256(av_rlp *r, const uint8_t be32[32]);
void av_rlp_list_begin(av_rlp *r);
void av_rlp_list_end(av_rlp *r);

/* Finish: AV_ERR if a list is still open or an allocation failed. The buffer stays owned by r. */
av_status av_rlp_finish(av_rlp *r, const uint8_t **out, size_t *len, av_err *e);

#endif /* AV_ARKIV_RLP_H */
