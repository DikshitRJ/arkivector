/*
 * config.h: daemon configuration (TOML file + environment overrides + defaults).
 *
 * Precedence, lowest to highest: built-in defaults (ARCHITECTURE section 16.3), TOML file,
 * environment ARKIVECTOR_<SECTION>_<KEY> (upper case; list values are comma separated), then
 * command-line flags applied by main.c. Unknown sections and keys are errors. Errors name the
 * file, the line (when the key is found in the text), the section and the key.
 *
 * Sections: [server] [arkiv] [storage] [sync] [write] [hnsw] [log] and [[collections]]. [signer] is
 * accepted and ignored (reserved for a later phase; a warning is logged).
 * [[collections]] tables are converted to JSON text, one string per table, and handed to the
 * registry unchanged (Lane C parses them with av_coldef_from_json).
 *
 * Ownership: av_config owns every string; free with av_config_free. Not thread-safe to mutate;
 * read-only after startup.
 */
#ifndef AV_CONFIG_H
#define AV_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

typedef struct av_config {
    struct {
        char *listen;     /* "127.0.0.1:8080" */
        int64_t threads;  /* query workers, 0 = auto (CPU count, 2..64) */
        int64_t max_body; /* bytes, default 32 MiB */
        int64_t max_connections;
        int64_t request_timeout_ms; /* slow-loris limit for receiving one request */
        int64_t idle_timeout_ms;    /* keep-alive idle limit */
        char *api_keys_file;        /* "" = auth off */
        double ratelimit_rps;       /* 0 = disabled */
        double ratelimit_burst;
    } server;
    struct {
        char **rpc_urls;
        size_t n_rpc_urls;
        char *ws_url;     /* "" = polling only */
        char *access_key; /* secret, never logged */
        int64_t chain_id;
        int64_t confirmations;
        int64_t max_log_range;
        double rpc_budget_per_sec;
        int64_t timeout_ms;
    } arkiv;
    struct {
        char *data_dir;
        int64_t map_size; /* LMDB map size in bytes */
    } storage;
    struct {
        int64_t bootstrap_parallelism;
        int64_t poll_interval_ms;
        int64_t reconcile_interval_s;
        int64_t ring_size; /* reorg ring depth in blocks */
    } sync;
    struct {
        double max_fee_cap_gwei;        /* 0 = no cap on maxFeePerGas */
        double priority_fee_percentile; /* eth_feeHistory reward percentile (default 50) */
        int64_t fee_history_blocks;     /* blocks of fee history (default 20) */
        double max_fee_gwei;            /* static fallback maxFeePerGas (default 20) */
        double priority_fee_gwei;       /* static fallback maxPriorityFeePerGas (default 1.5) */
        int64_t gas_margin_pct;         /* margin over eth_estimateGas (default 20) */
        int64_t fallback_gas_base; /* gas when eth_estimateGas fails: base + ops x per-op (default 60000) */
        int64_t replace_after_blocks; /* resend a stuck tx with bumped fees after N blocks (default 10) */
        int64_t max_replacements;     /* resends per tx, 0 = never replace (default 3) */
    } write;
    struct {
        int64_t persist;     /* 1 = save graphs under data_dir/segments and load them at startup */
        int64_t band_blocks; /* expiry band width in blocks; 0 = one band per collection */
    } hnsw;
    struct {
        char *level;  /* debug|info|warn|error */
        char *format; /* text|json */
    } log;
    char **collections_json; /* one JSON object text per [[collections]] table */
    size_t n_collections;

    /* Set from the command line by main.c, not from the file. */
    bool debug_routes;
    bool resync;
} av_config;

/* Fill defaults (strings allocated). Returns AV_ERR only on out of memory. */
av_status av_config_defaults(av_config *c, av_err *e);

/*
 * defaults, then the file at path (NULL = no file), then environment. On error c is left in a
 * freeable state (call av_config_free anyway).
 */
av_status av_config_load(av_config *c, const char *path, av_err *e);

/* Same as av_config_load with the TOML text given directly (tests). origin names it in errors. */
av_status av_config_load_string(av_config *c, const char *toml_text, const char *origin, av_err *e);

/* Environment only, on top of what is in c. */
av_status av_config_apply_env(av_config *c, av_err *e);

/* Check ranges and cross-field rules. Called by av_config_load; call again after CLI overrides. */
av_status av_config_validate(const av_config *c, av_err *e);

/* Replace a string field (takes a copy). */
av_status av_config_set_string(char **field, const char *value, av_err *e);

/* Log the effective configuration at info level (secrets omitted). */
void av_config_log_summary(const av_config *c);

void av_config_free(av_config *c);

#endif /* AV_CONFIG_H */
