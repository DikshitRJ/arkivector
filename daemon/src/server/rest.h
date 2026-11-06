/*
 * server/rest.h: REST route registration and the hooks other modules plug into.
 *
 * av_rest_register_all(router, debug) registers the operational routes (rest_ops.c) and then
 * calls every registrar added with av_rest_add_registrar, in order of addition. Later phases add
 * their route groups (collections, entities, search, ...) the same way, so main.c never has to
 * know about them.
 *
 * Thread safety: the setters below take an internal lock and may be called from any thread
 * (the sync engine calls av_rest_set_ready; handlers on query workers read). Registrars and the
 * sync provider are installed during startup, before the HTTP server listens.
 */
#ifndef AV_SERVER_REST_H
#define AV_SERVER_REST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server/router.h"

/* ---- registration ---- */

/* Called by av_rest_register_all. debug_routes mirrors --debug-routes (register /_debug/... only if true). */
typedef void (*av_rest_registrar)(av_router *r, bool debug_routes, void *ctx);
/* At most 16 registrars. Returns AV_ERR when full. */
av_status av_rest_add_registrar(av_rest_registrar fn, void *ctx, av_err *e);
/* Operational routes, then the registrars. */
av_status av_rest_register_all(av_router *r, bool debug_routes, av_err *e);

/* ---- readiness ---- */

/*
 * GET /readyz is 200 when ready, 503 otherwise. Before the first call the daemon is ready iff
 * every collection reported by the sync provider is "live" (so: ready with no collections).
 * After a call the explicit value wins (the sync engine sets it; shutdown sets false).
 */
void av_rest_set_ready(bool ready);

/* ---- sync status provider (Lane D) ---- */

typedef struct av_sync_status {
    const char *name;
    const char *state; /* init|bootstrap|catch_up|live|reorg_recovery|degraded */
    uint64_t as_of_block;
    uint64_t head_block;
    const char *last_error; /* NULL = JSON null */
} av_sync_status;

typedef void (*av_sync_visit_fn)(const av_sync_status *s, void *visit_ctx);
/* Must call visit once per collection, from the calling thread, with strings valid during the call. */
typedef void (*av_sync_provider_fn)(av_sync_visit_fn visit, void *visit_ctx, void *provider_ctx);
void av_rest_set_sync_provider(av_sync_provider_fn fn, void *ctx);

/* ---- version info ---- */

/* chain_id and writes_enabled for GET /v1/version. */
void av_rest_set_info(int64_t chain_id, bool writes_enabled);

/* Capability list for GET /v1/version: "ops.version", "ops.sync", "ops.metrics" plus anything added. */
av_status av_rest_add_capability(const char *capability, av_err *e);
/* Snapshot for tests; the pointer array is malloc'd, the strings are owned by the module. */
const char *const *av_rest_capabilities(size_t *n);

#endif /* AV_SERVER_REST_H */
