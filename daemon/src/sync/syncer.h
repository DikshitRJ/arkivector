/*
 * sync/syncer.h: the chain synchronisation engine (ARCHITECTURE §9, PLAN_DAEMON §6.4
 * steps 17-24).
 *
 * One sync thread serves every registered collection. It also applies changes (it is
 * the single LMDB writer), so it is the "applier" of the architecture. Per collection:
 *
 *   INIT -> BOOTSTRAP (no cursor / --resync / reorg deeper than the ring)
 *        -> CATCH_UP <-> LIVE    (eth_getLogs ledger in max_log_range chunks,
 *                                 batched state fetch, monotonic apply, publish)
 *        -> REORG_RECOVERY        (parentHash mismatch: walk the ring back to the
 *                                 common ancestor, re-fetch touched keys)
 *        -> DEGRADED              (RPC unreachable; reads keep the last as_of_block)
 *
 * Head detection: WebSocket newHeads as a doorbell, polling eth_blockNumber as the
 * fallback. Blocks behind head - confirmations are processed.
 * Reconciliation: a headless count probe every reconcile_interval_s; on mismatch, and
 * every sweep_interval_s, a full membership scan that re-applies every entity and
 * deletes local keys the chain no longer has.
 *
 * Thread safety: av_sync_start/stop from the main thread; av_sync_status and
 * av_sync_ready from any thread (internally locked).
 */
#ifndef AV_SYNC_SYNCER_H
#define AV_SYNC_SYNCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

struct av_rpc;
struct av_store;
struct av_registry;

typedef struct {
    struct av_rpc *rpc;
    struct av_store *store;
    struct av_registry *registry;
    const char *ws_url;     /* NULL: derive from the first RPC url; "" disables WS */
    const char *access_key; /* for WS; never logged */
    uint64_t confirmations;
    uint64_t max_log_range;
    int poll_interval_ms;
    int reconcile_interval_s;
    int sweep_interval_s;
    size_t ring_size;      /* K: reorg depth handled without re-bootstrap */
    uint64_t live_lag_blocks;
    size_t ttl_budget;     /* expired entities reclaimed per published block */
    int fetch_chunk;       /* initial OR-chunk size for state fetches (halved on -32004) */
    bool resync;           /* --resync: bootstrap every collection from scratch */
} av_sync_config;

typedef struct av_sync av_sync;

av_status av_sync_start(const av_sync_config *cfg, av_sync **out, av_err *e);
/* Stops at a batch boundary and joins the thread. NULL is a no-op. */
void av_sync_stop(av_sync *s);

typedef enum {
    AV_SYNC_INIT = 0,
    AV_SYNC_BOOTSTRAP,
    AV_SYNC_CATCH_UP,
    AV_SYNC_LIVE,
    AV_SYNC_REORG,
    AV_SYNC_DEGRADED
} av_sync_state;

const char *av_sync_state_name(av_sync_state st); /* spec SyncState strings */

typedef struct {
    char collection[64];
    av_sync_state state;
    uint64_t as_of_block;
    uint64_t head_block;
    uint64_t lag_blocks;
    uint64_t live_entities;
    uint64_t reorgs;
    uint64_t reconcile_divergence;
    char last_error[256];
} av_sync_coll_status;

/* Copy up to max entries; returns the number of collections. */
size_t av_sync_status_list(av_sync *s, av_sync_coll_status *out, size_t max);

struct av_sync_status;
void av_sync_rest_provider(void (*visit)(const struct av_sync_status *s, void *visit_ctx), void *visit_ctx,
                           void *provider_ctx);

/* True when every collection has finished its bootstrap (or there are none). */
bool av_sync_ready(av_sync *s);

/* Head block seen by the sync thread (0 before the first observation). */
uint64_t av_sync_head(av_sync *s);

#endif /* AV_SYNC_SYNCER_H */
