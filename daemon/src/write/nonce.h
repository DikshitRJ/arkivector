/*
 * write/nonce.h: Local nonce tracker with chain resync via RPC.
 */
#ifndef AV_WRITE_NONCE_H
#define AV_WRITE_NONCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/rpc.h"
#include "log.h"

typedef struct av_nonce_tracker av_nonce_tracker;

av_nonce_tracker *av_nonce_tracker_create(const uint8_t address[20], av_rpc *rpc);
void av_nonce_tracker_destroy(av_nonce_tracker *nt);

/* Acquire next nonce for broadcast */
av_status av_nonce_tracker_next(av_nonce_tracker *nt, uint64_t *out_nonce, av_err *e);

/* Resync local nonce from chain via eth_getTransactionCount (called at startup or on error) */
av_status av_nonce_tracker_resync(av_nonce_tracker *nt, av_err *e);

/* Peek current tracked nonce */
uint64_t av_nonce_tracker_current(const av_nonce_tracker *nt);

#endif /* AV_WRITE_NONCE_H */
