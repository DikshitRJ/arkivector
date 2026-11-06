/*
 * arkiv/ws.h: newHeads "doorbell" over a WebSocket (libcurl >= 8.11 curl_ws_* API).
 *
 * The daemon never trusts the pushed data: a head number only wakes the sync loop, which then
 * reads the chain over HTTP. Blocking, single-threaded use: one thread owns an av_ws handle
 * (no internal locking). Errors never kill the handle: av_ws_wait reconnects with exponential
 * backoff (200 ms .. 5 s) and returns -1 while disconnected, so the caller falls back to
 * polling eth_blockNumber.
 *
 * Secrets: the access key goes in the X-API-KEY handshake header; URLs are never logged.
 */
#ifndef AV_ARKIV_WS_H
#define AV_ARKIV_WS_H

#include <stdint.h>

#include "log.h"

typedef struct av_ws av_ws;

/* url: ws://, wss://, http:// or https:// (http(s) is converted to ws(s)). Tries one connect and
   one eth_subscribe("newHeads"); a failed connect is NOT an error (the handle starts
   disconnected). Returns NULL only for an invalid url or OOM. access_key may be NULL. */
av_ws *av_ws_open(const char *url, const char *access_key, av_err *e);

/* Wait up to timeout_ms for a new head. Returns 1 with *head set, 0 on timeout, -1 if
   disconnected (reconnect attempts are rate-limited by backoff and happen inside this call). */
int av_ws_wait(av_ws *w, int timeout_ms, uint64_t *head);

/* True when the socket is currently connected and subscribed. */
int av_ws_connected(const av_ws *w);

void av_ws_close(av_ws *w);

#endif /* AV_ARKIV_WS_H */
