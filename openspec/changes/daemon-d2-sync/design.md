# Design: daemon-d2-sync (lane contracts)

PLAN_DAEMON.md §6 is the task list. This file fixes the **interfaces between the D2 lanes** so they can be built in parallel. A lane may add functions, but must not change these signatures without the lead.

## Decision: sync uses blocking libcurl on its own thread
The plan sketches curl-multi driven by the libuv loop. Instead, chain I/O runs on a dedicated **sync thread** (one per daemon, serving all collections), using **blocking libcurl easy handles with timeouts** plus `curl_ws_*` for the WebSocket doorbell with poll timeouts. The libuv loop thread only does HTTP. This keeps the loop non-blocking (the architectural requirement), avoids callback state machines, and makes the RPC client testable in isolation. HTTP handlers that need chain reads in D3+ (follow-by-manifest, verify) call the same blocking client from a query worker.

## Threads
- **loop** (libuv): HTTP accept/parse/respond. It hands each request to the query pool and gets the response back through an mpsc queue plus `uv_async_send`.
- **query pool** (N workers): run handlers.
- **sync** (1): the per-collection state machine, RPC, building `av_apply_batch`.
- **applier** (1): consumes batches, LMDB txn, index updates, publish.

## Lane B: Arkiv client (`src/arkiv/`)
```c
/* rpc.h */
typedef struct av_rpc av_rpc;
typedef struct { const char *const *urls; size_t n_urls; const char *access_key; int timeout_ms;
                 int max_retries; double budget_per_sec; uint64_t chain_id; } av_rpc_config;
av_rpc   *av_rpc_create(const av_rpc_config *cfg, av_err *e);
void      av_rpc_destroy(av_rpc *r);
/* One JSON-RPC call; params_json is a JSON array text. On success *doc owns the response
   (caller yyjson_doc_free) and *result points into it. RPC errors: AV_ERR, e->code
   "arkiv_rpc_error", *rpc_code = JSON-RPC error code (or 0 for transport/HTTP errors).
   Retries transport/429/5xx with jittered backoff; never -32003/-32004/-32602. Thread-safe. */
av_status av_rpc_call(av_rpc *r, const char *method, const char *params_json,
                      struct yyjson_doc **doc, struct yyjson_val **result, int *rpc_code, av_err *e);
av_status av_rpc_block_number(av_rpc *r, uint64_t *out, av_err *e);
typedef struct { uint64_t number; uint8_t hash[32]; uint8_t parent_hash[32]; uint64_t timestamp; } av_block;
av_status av_rpc_get_block(av_rpc *r, uint64_t number, av_block *out, av_err *e);
typedef struct { uint64_t current_block; uint64_t current_block_time; uint64_t block_duration; } av_block_timing;
av_status av_rpc_block_timing(av_rpc *r, av_block_timing *out, av_err *e);
/* arkiv_query: query string, full projection (key, owner, creator, createdAt, updatedAt,
   expiresAt, attributes:true) unless key_only; cursor NULL for the first page. Entities in arena a. */
typedef struct { av_entity *ents; size_t n; char *next_cursor; uint64_t block; } av_query_page;
av_status av_rpc_query(av_rpc *r, const char *query, const char *cursor, int limit, bool key_only,
                       av_arena *a, av_query_page *out, int *rpc_code, av_err *e);
av_status av_rpc_get_entity(av_rpc *r, const uint8_t key[32], av_arena *a, av_entity *out, bool *found, av_err *e);
/* eth_getLogs over [from,to] for the Arkiv contract, topics filter as JSON (NULL = the 5 event topic0s). */
av_status av_rpc_get_logs(av_rpc *r, uint64_t from, uint64_t to, const char *topics_json, av_arena *a,
                          struct av_event **evs, size_t *n, int *rpc_code, av_err *e);
/* logs.h */
typedef enum { AV_EV_CREATED = 1, AV_EV_PATCHED, AV_EV_DELETED, AV_EV_EXTENDED, AV_EV_TRANSFERRED } av_event_kind;
typedef struct av_event { av_event_kind kind; uint8_t key[32]; uint8_t owner[20]; uint8_t prev_owner[20];
    uint8_t new_owner[20]; uint64_t expires_at; uint32_t flags; uint64_t block; uint32_t log_index;
    uint8_t block_hash[32]; uint8_t tx_hash[32]; bool removed; } av_event;
const uint8_t *av_event_topic0(av_event_kind k); /* 32 bytes, computed with keccak at init, asserted == docs */
/* ws.h: newHeads doorbell; av_ws_wait returns 1 with *head set, 0 on timeout, -1 if disconnected
   (caller falls back to polling eth_blockNumber). Reconnects with backoff inside av_ws_wait. */
typedef struct av_ws av_ws;
av_ws *av_ws_open(const char *url, const char *access_key, av_err *e);
int    av_ws_wait(av_ws *w, int timeout_ms, uint64_t *head);
void   av_ws_close(av_ws *w);
/* query_builder.h: safe query strings (str('..') with '' escaping, key()/addr() literals,
   OR chains). Fixed-size output buffers; AV_ERR if the result exceeds max_len. */
/* keccak.h */ void av_keccak256(const void *data, size_t len, uint8_t out[32]);
```
Arkiv contract address: `0x4400000000000000000000000000000000000044` (the research docs; the mock uses it). Chain id default `0x76edaf`.

## Lane C: storage and index (`src/store/`, `src/index/`, `src/core/collection.*`, `src/core/registry.*`, `src/core/coldef.c`)
```c
/* store/lmdb_store.h: one LMDB env per data dir; single writer (applier). */
typedef struct av_store av_store;
av_status av_store_open(const char *dir, size_t map_size, av_store **out, av_err *e);
void      av_store_close(av_store *s);
typedef struct av_txn av_txn;                 /* write txn (applier only) */
av_status av_store_begin(av_store *s, av_txn **t, av_err *e);
av_status av_store_commit(av_txn *t, av_err *e);
void      av_store_abort(av_txn *t);
/* collections */
av_status av_store_put_collection(av_txn *t, const char *name, const char *json, size_t len, av_err *e);
av_status av_store_del_collection(av_txn *t, const char *name, av_err *e); /* drops all its DBs */
typedef bool (*av_store_coll_cb)(const char *name, const char *json, size_t len, void *ctx);
av_status av_store_list_collections(av_store *s, av_store_coll_cb cb, void *ctx, av_err *e);
/* entity rows: key32 -> packed row */
typedef struct { uint32_t docid; uint8_t owner[20]; uint8_t creator[20]; uint64_t created_at, updated_at, expires_at;
                 uint8_t vec_hash[32]; const uint8_t *attrs_blob; size_t attrs_len; } av_row;
av_status av_store_put_entity(av_txn *t, const char *coll, const uint8_t key[32], const av_row *row, av_err *e);
av_status av_store_del_entity(av_txn *t, const char *coll, const uint8_t key[32], av_err *e);
av_status av_store_get_entity(av_store *s, const char *coll, const uint8_t key[32], av_arena *a, av_row *out, bool *found, av_err *e);
typedef bool (*av_store_row_cb)(const uint8_t key[32], const av_row *row, void *ctx);
av_status av_store_scan_entities(av_store *s, const char *coll, av_store_row_cb cb, void *ctx, av_err *e);
/* external id -> key, cursor, reorg ring, quarantine */
av_status av_store_put_id(av_txn *t, const char *coll, const char *id, size_t idlen, const uint8_t key[32], av_err *e);
av_status av_store_del_id(av_txn *t, const char *coll, const char *id, size_t idlen, av_err *e);
av_status av_store_get_id(av_store *s, const char *coll, const char *id, size_t idlen, uint8_t key[32], bool *found, av_err *e);
typedef struct { uint64_t block; uint8_t hash[32]; } av_cursor;
av_status av_store_put_cursor(av_txn *t, const char *coll, const av_cursor *c, av_err *e);
av_status av_store_get_cursor(av_store *s, const char *coll, av_cursor *c, bool *found, av_err *e);
av_status av_store_ring_put(av_txn *t, const char *coll, uint64_t block, const uint8_t hash[32],
                            const uint8_t (*keys)[32], size_t n_keys, av_err *e);
/* ring_get returns hash + touched keys of a block; ring_trim keeps the last K blocks */
av_status av_store_quarantine(av_txn *t, const char *coll, const uint8_t key[32], const char *reason, av_err *e);
/* attrs blob = serialized av_attr list: av_attrs_pack/unpack in store/schema.h */

/* index/docid.h */ docid <-> key32 maps (hash map + dense array), next_docid counter.
/* index/live.h  */ roaring live bitmap with copy-on-publish (readers keep a snapshot pointer).
/* index/attr_index.h: per (name,type): sorted value vector -> roaring postings; exists[name],
   typed[(name,type)]; insert/remove of one docid's attributes. Query side arrives in D3. */
/* index/segment.h: PLAN_DAEMON §3 interface. D2 implements AV_SEG_FLAT (vectors decoded into an
   arena_file at 8-byte alignment + docid -> offset table) and av_segment_scan_exact. */

/* core/collection.h */
typedef struct av_collection av_collection;   /* definition + segments + live + attr index + docid
   maps + as_of_block + publish rwlock + condvar (PLAN_DAEMON §3) */
av_collection *av_collection_create(const av_coldef *def, av_store *store, const char *data_dir, av_err *e);
void      av_collection_destroy(av_collection *c);
const av_coldef *av_collection_def(const av_collection *c);
uint64_t  av_collection_as_of(const av_collection *c);
void      av_collection_read_lock(av_collection *c);
void      av_collection_read_unlock(av_collection *c);
av_status av_collection_wait_block(av_collection *c, uint64_t min_block, int timeout_ms, av_err *e);
/* applier-side mutation API (single thread), applied between begin/publish: */
av_status av_collection_upsert(av_collection *c, av_txn *t, const av_entity *ent, bool *changed, av_err *e);
     /* monotonic (updated_at >= stored), membership, av.vec validation (quarantine on failure),
        vector changed -> new docid + tombstone old; attrs-only -> postings update */
av_status av_collection_delete(av_collection *c, av_txn *t, const uint8_t key[32], av_err *e);
av_status av_collection_extend(av_collection *c, av_txn *t, const uint8_t key[32], uint64_t expires_at, av_err *e);
av_status av_collection_transfer(av_collection *c, av_txn *t, const uint8_t key[32], const uint8_t new_owner[20], av_err *e);
void      av_collection_publish(av_collection *c, uint64_t block); /* after commit: swap live, set as_of, broadcast */
size_t    av_collection_live_count(const av_collection *c);
/* debug routes */
typedef bool (*av_collection_key_cb)(const uint8_t key[32], uint64_t updated_at, uint64_t expires_at, void *ctx);
void      av_collection_each_live(av_collection *c, av_collection_key_cb cb, void *ctx);   /* under read lock, sorted by key */
/* core/registry.h: name -> collection (rwlock), persisted via av_store_put_collection. */
```

## Lane A: runtime and HTTP (`src/config.*`, `src/log.*`, `src/core/{threadpool,mpsc}.*`, `src/server/*`, `src/main.c`)
```c
/* server/http.h */
typedef struct av_http_req { const char *method; const char *path; const char *query; /* raw */
    const char *body; size_t body_len; const char *request_id; /* ULID */
    /* headers: */ const char *content_type; const char *authorization; const char *idempotency_key;
    av_arena *arena; /* request arena, reset after the response */ } av_http_req;
typedef struct av_http_resp { int status; const char *content_type; char *body; size_t body_len;
    /* extra headers */ uint64_t as_of_block; bool has_as_of; const char *retry_after; } av_http_resp;
typedef void (*av_http_handler)(const av_http_req *req, const char *const *params, size_t n_params,
                                av_http_resp *resp, void *ctx);   /* runs on a query worker */
/* router.h: av_router_add(r, "GET", "/v1/collections/{name}/entities/{ref}", handler, ctx);
   colon verbs ("/v1/collections:follow", "/v1/collections/{name}/search:batch") are literal suffixes.
   404 route_not_found / 405 with Allow. */
/* problem.h: av_problem_from_err(const av_err *e, av_http_resp *resp, const char *instance) ->
   RFC 9457 body {type: "urn:arkivector:error:<code>", title, status, detail, code, request_id, + field,
   vb_error, position, as_of_block}; status table generated at build time from spec/error-codes.json. */
/* Every response carries X-Request-Id, Arkivector-Api-Version: 1.0.0 and, when known,
   Arkivector-As-Of-Block. */
```
Config: TOML (`arkivector.example.toml`, ARCHITECTURE §16.3) plus env `ARKIVECTOR_<SECTION>_<KEY>`. `[[collections]]` entries are loaded at start.

## Lane D: sync (lead, after the A/B/C merges)
`src/sync/{syncer,bootstrap,discovery,apply,ttl,reorg,reconcile}.c`, `store/checkpoint.c`, built on the APIs above.
