/*
 * arkiv/rpc.h: blocking JSON-RPC 2.0 client for the Arkiv node (libcurl easy, one handle per call).
 *
 * Threading: designed to be called from the sync thread (and tests); every function is thread-safe
 * (per-call curl handle, mutex-guarded endpoint health, atomic request ids). Calls block the caller.
 *
 * Resilience (ARCHITECTURE §9.9): token-bucket budget with per-method CU weights; failover across
 * urls (an endpoint that fails at the transport level or answers 5xx is skipped for a backoff
 * period); retries with jittered exponential backoff for transport errors, HTTP 429 and 5xx only.
 * JSON-RPC errors (-32003, -32004, -32602, -32005, ...) are deterministic and are NEVER retried.
 *
 * Secrets: the access key is sent in the X-API-KEY header only. Endpoint URLs may embed a key in
 * the path, so errors and logs refer to endpoints by index and never print URLs or keys.
 *
 * Errors: AV_ERR with e->code "arkiv_rpc_error" (upstream/transport/protocol failures),
 * "invalid_argument" (bad caller input) or "internal". *rpc_code is the JSON-RPC error code, or 0
 * for transport/HTTP/protocol errors.
 */
#ifndef AV_ARKIV_RPC_H
#define AV_ARKIV_RPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arkiv/entity.h"
#include "arkiv/logs.h"
#include "core/arena.h"
#include "log.h"

struct yyjson_doc;
struct yyjson_val;

#define AV_RPC_MAX_RESPONSE     ((size_t) 256 * 1024 * 1024)
#define AV_RPC_DEFAULT_CONTRACT "0x4400000000000000000000000000000000000044"

typedef struct av_rpc av_rpc;

typedef struct {
    const char *const *urls; /* http:// or https:// endpoints, tried in order */
    size_t n_urls;
    const char *access_key; /* X-API-KEY header; NULL or "" = none */
    int timeout_ms;         /* per HTTP attempt; <= 0 selects 10000 */
    int max_retries;        /* retries after the first attempt (clamped to 0..10) */
    double budget_per_sec;  /* CU per second; <= 0 = unlimited */
    uint64_t chain_id;
    /* Added in D2 Lane B (append-only; zero/NULL keeps the old behaviour): */
    const char *contract; /* Arkiv contract address for eth_getLogs; NULL = AV_RPC_DEFAULT_CONTRACT */
} av_rpc_config;

/* Copies cfg (strings included). Returns NULL and fills e on invalid config or OOM. */
av_rpc *av_rpc_create(const av_rpc_config *cfg, av_err *e);
void av_rpc_destroy(av_rpc *r);

/* One JSON-RPC call; params_json is a JSON array text (NULL = "[]"). On success *doc owns the
   response (caller yyjson_doc_free) and *result points into it. Thread-safe. */
av_status av_rpc_call(av_rpc *r, const char *method, const char *params_json, struct yyjson_doc **doc,
                      struct yyjson_val **result, int *rpc_code, av_err *e);

av_status av_rpc_block_number(av_rpc *r, uint64_t *out, av_err *e);

typedef struct {
    uint64_t number;
    uint8_t hash[32];
    uint8_t parent_hash[32];
    uint64_t timestamp;
} av_block;
/* AV_ERR "arkiv_rpc_error" ("block N not found") when the node returns null. */
av_status av_rpc_get_block(av_rpc *r, uint64_t number, av_block *out, av_err *e);

typedef struct {
    uint64_t current_block;
    uint64_t current_block_time;
    uint64_t block_duration;
} av_block_timing;
av_status av_rpc_block_timing(av_rpc *r, av_block_timing *out, av_err *e);

/* arkiv_query with key-cursor paging. `query` is the BASE predicate (may be NULL/"" for all
   entities; built with arkiv/query_builder.h); the client always sends
   "(<query>) AND $key > key(<cursor>) ORDER BY $key ASC" (no AND-part for the first page), so
   pages are exactly-once under concurrent writes. limit is 1..100. The projection is the full
   entity (key, owner, creator, createdAt, updatedAt, expiresAt, attributes) unless key_only
   (key only). next_cursor is the "0x.." key of the last entity when the page was full
   (n == limit), else NULL (iteration done); it lives in arena a, as do the entities.
   The page is validated: keys strictly ascending and after the cursor. */
typedef struct {
    av_entity *ents;
    size_t n;
    char *next_cursor;
    uint64_t block;
} av_query_page;
av_status av_rpc_query(av_rpc *r, const char *query, const char *cursor, int limit, bool key_only,
                       av_arena *a, av_query_page *out, int *rpc_code, av_err *e);

/* arkiv_query with select.count: true (headless count). */
av_status av_rpc_count(av_rpc *r, const char *query, uint64_t as_of, uint64_t *out_count, av_err *e);

/* arkiv_getEntity. *found = false (AV_OK) when the node returns null (absent or expired). */
av_status av_rpc_get_entity(av_rpc *r, const uint8_t key[32], av_arena *a, av_entity *out, bool *found,
                            av_err *e);

/* eth_getLogs over [from,to] for the Arkiv contract. topics_json is the JSON "topics" array
   (NULL = [[the 5 event topic0s]]). Unknown topic0 entries are skipped. A node range limit
   (-32005) comes back as AV_ERR with *rpc_code = -32005 so the caller can halve the range. */
av_status av_rpc_get_logs(av_rpc *r, uint64_t from, uint64_t to, const char *topics_json, av_arena *a,
                          struct av_event **evs, size_t *n, int *rpc_code, av_err *e);

/* Diagnostics: total HTTP attempts made by this client over all calls (retries included). */
uint64_t av_rpc_attempts(av_rpc *r);

#endif /* AV_ARKIV_RPC_H */
