/*
 * main.c: arkivectord entry point.
 *
 * Startup order: arguments -> config -> log -> kernel dispatch -> services (store, registry,
 * sync: hooks for Lane D) -> HTTP listen. Shutdown on SIGTERM/SIGINT: stop accepting, drain
 * in-flight requests, stop services, exit 0. A second signal exits immediately with status 130.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

#include "av_licenses.h"
#include "config.h"
#include "kernels/dispatch.h"
#include "log.h"
#include "server/http.h"
#include "server/metrics.h"
#include "server/rest.h"
#include "server/router.h"
#include "version.h"
#include "arkiv/rpc.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "store/lmdb_store.h"
#include "sync/syncer.h"
#include "arkiv/signer.h"
#include "core/fileio.h"
#include "server/idempotency.h"
#include "write/writer.h"

typedef struct {
    const char *config_path;
    const char *data_dir;
    const char *listen;
    bool debug_routes;
    bool resync;
    bool show_version;
    bool show_licenses;
    bool show_help;
} cli_opts;

typedef struct {
    uv_signal_t sigterm;
    uv_signal_t sigint;
    av_http_server *http;
    int signals_seen;
    bool http_done;
} app_state;

static void usage(FILE *out)
{
    fprintf(out, "usage: arkivectord [options]\n"
                 "  --config <file>     TOML configuration file (see arkivector.example.toml)\n"
                 "  --data-dir <dir>    overrides [storage] data_dir\n"
                 "  --listen <host:port> overrides [server] listen\n"
                 "  --debug-routes      enable /_debug/... routes (testing only)\n"
                 "  --resync            discard stored sync state and bootstrap again\n"
                 "  --version           print the version and exit\n"
                 "  --licenses          print the licences of copied upstream code and exit\n"
                 "  --help              print this help and exit\n"
                 "Environment: ARKIVECTOR_<SECTION>_<KEY> overrides the file (for example "
                 "ARKIVECTOR_LOG_LEVEL=debug).\n");
}

static int parse_args(int argc, char **argv, cli_opts *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **target = NULL;
        if (strcmp(a, "--config") == 0) {
            target = &o->config_path;
        } else if (strcmp(a, "--data-dir") == 0) {
            target = &o->data_dir;
        } else if (strcmp(a, "--listen") == 0) {
            target = &o->listen;
        } else if (strcmp(a, "--debug-routes") == 0) {
            o->debug_routes = true;
        } else if (strcmp(a, "--resync") == 0) {
            o->resync = true;
        } else if (strcmp(a, "--version") == 0) {
            o->show_version = true;
        } else if (strcmp(a, "--licenses") == 0) {
            o->show_licenses = true;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            o->show_help = true;
        } else {
            fprintf(stderr, "arkivectord: unknown argument '%s'\n", a);
            usage(stderr);
            return -1;
        }
        if (target != NULL) {
            if (i + 1 >= argc) {
                fprintf(stderr, "arkivectord: %s needs a value\n", a);
                return -1;
            }
            *target = argv[++i];
        }
    }
    return 0;
}

/* ---- hooks for Lane D (store, registry, sync) ---- */

static av_store *g_store = NULL;
static av_registry *g_registry = NULL;
static av_rpc *g_rpc = NULL;
static av_sync *g_sync = NULL;
static av_writer *g_writer = NULL;
static av_idempotency_store *g_idem = NULL;
static av_signer *g_signer = NULL;

/* Keystore passphrase from passphrase_file or the systemd credential $CREDENTIALS_DIRECTORY/<name>.
 * The first line is used (a trailing CR/LF is stripped). The buffer is wiped by the caller. */
static av_status read_passphrase(const av_config *cfg, char *out, size_t cap, av_err *e)
{
    char path[4096];
    if (cfg->signer.passphrase_credential[0] != '\0') {
        const char *dir = getenv("CREDENTIALS_DIRECTORY");
        if (dir == NULL || dir[0] == '\0') {
            av_err_set(
                e, "invalid_argument",
                "write.signer.passphrase_credential is set but CREDENTIALS_DIRECTORY is not (run under "
                "systemd with LoadCredential=/SetCredentialEncrypted=)");
            return AV_ERR;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, cfg->signer.passphrase_credential);
    } else {
        snprintf(path, sizeof(path), "%s", cfg->signer.passphrase_file);
    }
    size_t n = 0;
    char *buf = av_read_file(path, "keystore passphrase", cap - 1, &n, e);
    if (buf == NULL)
        return AV_ERR;
    size_t len = strcspn(buf, "\r\n");
    memcpy(out, buf, len);
    out[len] = '\0';
    explicit_bzero(buf, n);
    free(buf);
    return AV_OK;
}

/*
 * [write.signer] (GitHub issue #12). kind "auto" keeps the historical behaviour: sign with the key in
 * env_var (ARKIV_PRIVATE_KEY, or the legacy ARKIVECTOR_SIGNER_KEY) when it is set, otherwise run
 * read-only; a bad key there only logs a warning. An explicitly chosen kind that fails stops startup.
 */
static av_status create_signer(const av_config *cfg, av_signer **out, av_err *e)
{
    *out = NULL;
    const char *kind = cfg->signer.kind;
    if (strcmp(kind, "none") == 0) {
        av_log(AV_LOG_INFO, "signer", "signer disabled: managed writes are off (prepare/submit still work)");
        return AV_OK;
    }
    if (strcmp(kind, "auto") == 0) {
        const char *vars[2] = {cfg->signer.env_var, "ARKIVECTOR_SIGNER_KEY"};
        for (size_t i = 0; i < 2; i++) {
            const char *v = getenv(vars[i]);
            if (v == NULL || v[0] == '\0')
                continue;
            av_err s_err;
            av_err_clear(&s_err);
            if (av_signer_create_env(vars[i], out, &s_err) == AV_OK)
                break;
            av_log(AV_LOG_WARN, "signer", "ignoring %s: %s", vars[i], s_err.msg);
        }
    } else if (strcmp(kind, "env") == 0) {
        if (av_signer_create_env(cfg->signer.env_var, out, e) != AV_OK)
            return AV_ERR;
    } else if (strcmp(kind, "keystore") == 0) {
        char pass[1024];
        if (read_passphrase(cfg, pass, sizeof(pass), e) != AV_OK)
            return AV_ERR;
        av_status st = av_signer_create_keystore(cfg->signer.keystore_path, pass, out, e);
        explicit_bzero(pass, sizeof(pass));
        if (st != AV_OK)
            return AV_ERR;
    } else if (strcmp(kind, "remote") == 0) {
        av_signer_remote_config rc;
        memset(&rc, 0, sizeof(rc));
        rc.url = cfg->signer.remote_url;
        rc.address = cfg->signer.remote_address;
        rc.method = cfg->signer.remote_method;
        rc.chain_id = (uint64_t) cfg->arkiv.chain_id;
        rc.timeout_ms = (int) cfg->signer.remote_timeout_ms;
        if (av_signer_create_remote_ex(&rc, out, e) != AV_OK)
            return AV_ERR;
    }
    if (*out != NULL) {
        char addr[43];
        av_signer_get_address_hex(*out, addr);
        av_log(AV_LOG_INFO, "signer", "managed writes enabled: %s signer, address %s",
               av_signer_kind_name(*out), addr);
    } else {
        av_log(AV_LOG_INFO, "signer",
               "no signer configured: managed writes are off (prepare/submit still work)");
    }
    return AV_OK;
}

static av_status services_start(const av_config *cfg, av_err *e)
{
    /* 1. Open LMDB store */
    size_t map_size = cfg->storage.map_size > 0 ? (size_t) cfg->storage.map_size : 0;
    if (av_store_open(cfg->storage.data_dir, map_size, &g_store, e) != AV_OK)
        return AV_ERR;

    /* 2. Load registry */
    av_collection_set_hnsw_config(cfg->hnsw.persist != 0, (uint64_t) cfg->hnsw.band_blocks);
    g_registry = av_registry_create(g_store, cfg->storage.data_dir, e);
    if (g_registry == NULL)
        return AV_ERR;

    /* 3. Register configured static collections if any */
    av_arena *parse_arena = av_arena_create("coldef_parse", 64 * 1024, 0);
    for (size_t i = 0; i < cfg->n_collections; i++) {
        av_coldef def;
        memset(&def, 0, sizeof(def));
        if (av_coldef_from_text(cfg->collections_json[i], strlen(cfg->collections_json[i]), parse_arena,
                                false, &def, e) == AV_OK) {
            av_collection *c = NULL;
            av_err ign;
            av_err_clear(&ign);
            (void) av_registry_add(g_registry, &def, &c, &ign);
            if (c != NULL)
                av_collection_release(c);
        }
        av_arena_reset(parse_arena);
    }
    av_arena_destroy(parse_arena);

    /* 4. Create RPC client */
    av_rpc_config rpc_cfg = {
        .urls = (const char *const *) cfg->arkiv.rpc_urls,
        .n_urls = cfg->arkiv.n_rpc_urls,
        .access_key = cfg->arkiv.access_key,
        .timeout_ms = (int) cfg->arkiv.timeout_ms,
        .budget_per_sec = cfg->arkiv.rpc_budget_per_sec,
        .chain_id = (uint64_t) cfg->arkiv.chain_id,
        .contract = NULL,
    };
    g_rpc = av_rpc_create(&rpc_cfg, e);
    if (g_rpc == NULL)
        return AV_ERR;

    /* 5. Start syncer */
    av_sync_config sync_cfg = {
        .rpc = g_rpc,
        .store = g_store,
        .registry = g_registry,
        .ws_url = cfg->arkiv.ws_url,
        .access_key = cfg->arkiv.access_key,
        .confirmations = (uint64_t) (cfg->arkiv.confirmations > 0 ? cfg->arkiv.confirmations : 0),
        .max_log_range = (uint64_t) (cfg->arkiv.max_log_range > 0 ? cfg->arkiv.max_log_range : 2000),
        .poll_interval_ms = (int) (cfg->sync.poll_interval_ms > 0 ? cfg->sync.poll_interval_ms : 1000),
        .reconcile_interval_s =
            (int) (cfg->sync.reconcile_interval_s > 0 ? cfg->sync.reconcile_interval_s : 60),
        .sweep_interval_s = 21600,
        .ring_size = (size_t) (cfg->sync.ring_size > 0 ? cfg->sync.ring_size : 32),
        .live_lag_blocks = 2,
        .ttl_budget = 10000,
        .fetch_chunk = 32,
        .resync = cfg->resync,
    };
    if (av_sync_start(&sync_cfg, &g_sync, e) != AV_OK)
        return AV_ERR;

    av_rest_set_sync_provider(av_sync_rest_provider, g_sync);

    /* 6. Idempotency store */
    g_idem = av_idempotency_store_create(g_store);

    /* 7. Signer and Writer */
    g_signer = NULL;
    if (create_signer(cfg, &g_signer, e) != AV_OK)
        return AV_ERR;

    av_writer_config wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.chain_id = (uint64_t) cfg->arkiv.chain_id;
    wcfg.gas_limit = (uint64_t) cfg->write.fallback_gas_base;
    wcfg.max_fee_per_gas = (uint64_t) (cfg->write.max_fee_gwei * 1e9);
    wcfg.max_priority_fee_per_gas = (uint64_t) (cfg->write.priority_fee_gwei * 1e9);
    wcfg.max_fee_cap = (uint64_t) (cfg->write.max_fee_cap_gwei * 1e9);
    wcfg.priority_fee_percentile = cfg->write.priority_fee_percentile;
    wcfg.fee_history_blocks = (uint32_t) cfg->write.fee_history_blocks;
    wcfg.gas_margin_pct = (uint32_t) cfg->write.gas_margin_pct;
    wcfg.replace_after_blocks = (uint32_t) cfg->write.replace_after_blocks;
    wcfg.max_replacements = (uint32_t) cfg->write.max_replacements;
    wcfg.no_replace = cfg->write.max_replacements == 0;
    g_writer = av_writer_create(&wcfg, g_signer, g_rpc, g_store, g_registry, e);
    av_rest_set_writer(g_writer, g_idem);
    av_rest_set_info(cfg->arkiv.chain_id, g_signer != NULL);
    if (g_signer != NULL) {
        char addr[43];
        av_signer_get_address_hex(g_signer, addr);
        av_rest_set_signer(av_signer_kind_name(g_signer), addr);
    }

    return AV_OK;
}

static void services_stop(void)
{
    if (g_writer != NULL) {
        av_writer_destroy(g_writer);
        g_writer = NULL;
    }
    if (g_signer != NULL) {
        av_signer_destroy(g_signer);
        g_signer = NULL;
    }
    if (g_idem != NULL) {
        av_idempotency_store_destroy(g_idem);
        g_idem = NULL;
    }
    if (g_sync != NULL) {
        av_sync_stop(g_sync);
        g_sync = NULL;
    }
    if (g_rpc != NULL) {
        av_rpc_destroy(g_rpc);
        g_rpc = NULL;
    }
    if (g_registry != NULL) {
        av_registry_destroy(g_registry);
        g_registry = NULL;
    }
    if (g_store != NULL) {
        av_store_close(g_store);
        g_store = NULL;
    }
}

/* ---- startup helpers ---- */

static av_status ensure_data_dir(const char *dir, av_err *e)
{
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            av_err_set(e, "invalid_argument", "data_dir %s exists and is not a directory", dir);
            return AV_ERR;
        }
        if (access(dir, R_OK | W_OK | X_OK) != 0) {
            av_err_set(e, "invalid_argument", "data_dir %s is not readable and writable", dir);
            return AV_ERR;
        }
        return AV_OK;
    }
    if (mkdir(dir, 0700) != 0) {
        av_err_set(e, "invalid_argument", "cannot create data_dir %s: %s", dir, strerror(errno));
        return AV_ERR;
    }
    return AV_OK;
}

static size_t worker_count(const av_config *cfg)
{
    if (cfg->server.threads > 0) {
        return (size_t) cfg->server.threads;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) {
        n = 2;
    }
    if (n > 64) {
        n = 64;
    }
    return (size_t) n;
}

/* ---- signals and shutdown ---- */

static void http_done(void *ctx)
{
    app_state *app = (app_state *) ctx;
    app->http_done = true;
    uv_close((uv_handle_t *) &app->sigterm, NULL);
    uv_close((uv_handle_t *) &app->sigint, NULL);
}

static void on_signal(uv_signal_t *h, int signum)
{
    app_state *app = (app_state *) h->data;
    app->signals_seen++;
    if (app->signals_seen > 1) {
        av_log(AV_LOG_ERROR, "main", "second signal (%d): exiting immediately", signum);
        _exit(130);
    }
    av_log(AV_LOG_INFO, "main", "signal %d received: shutting down gracefully", signum);
    av_rest_set_ready(false);
    av_http_shutdown(app->http, http_done, app);
}

int main(int argc, char **argv)
{
    cli_opts opts;
    if (parse_args(argc, argv, &opts) != 0) {
        return 2;
    }
    if (opts.show_help) {
        usage(stdout);
        return 0;
    }
    if (opts.show_version) {
        printf("%s\n", av_version_string());
        return 0;
    }
    if (opts.show_licenses) {
        printf("=== pgvector (LICENSE.pgvector) ===\n%s\n=== PostgreSQL (LICENSE.postgresql) ===\n%s\n",
               (const char *) av_license_pgvector, (const char *) av_license_postgresql);
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);

    av_err e;
    av_err_clear(&e);
    av_config cfg;
    if (av_config_load(&cfg, opts.config_path, &e) != AV_OK) {
        fprintf(stderr, "arkivectord: %s\n", e.msg);
        av_config_free(&cfg);
        return 2;
    }
    if ((opts.listen != NULL && av_config_set_string(&cfg.server.listen, opts.listen, &e) != AV_OK) ||
        (opts.data_dir != NULL && av_config_set_string(&cfg.storage.data_dir, opts.data_dir, &e) != AV_OK) ||
        av_config_validate(&cfg, &e) != AV_OK) {
        fprintf(stderr, "arkivectord: %s\n", e.msg);
        av_config_free(&cfg);
        return 2;
    }
    cfg.debug_routes = opts.debug_routes;
    cfg.resync = opts.resync;

    av_log_level level = AV_LOG_INFO;
    av_log_format format = AV_LOG_TEXT;
    (void) av_log_parse_level(cfg.log.level, &level);
    (void) av_log_parse_format(cfg.log.format, &format);
    av_log_set_level(level);
    av_log_set_format(format);
    av_log(AV_LOG_INFO, "main", "starting %s", av_version_string());
    av_config_log_summary(&cfg);

    av_kernels_init();
    {
        static const char *const tier_label[] = {"tier"};
        const char *tier = av_kernel_tier();
        const char *lv[1] = {tier};
        av_metric_set(av_metric_register("arkivector_kernel_tier", "Selected distance kernel tier (value 1).",
                                         AV_MTYPE_GAUGE, tier_label, 1),
                      lv, 1);
        av_log(AV_LOG_INFO, "main", "kernel tier: %s", tier);
    }

    int exit_code = 1;
    av_router *router = NULL;
    app_state app;
    memset(&app, 0, sizeof(app));
    uv_loop_t *loop = uv_default_loop();
    bool services_up = false;

    if (ensure_data_dir(cfg.storage.data_dir, &e) != AV_OK) {
        av_log(AV_LOG_ERROR, "main", "%s", e.msg);
        goto out;
    }
    if (services_start(&cfg, &e) != AV_OK) {
        av_log(AV_LOG_ERROR, "main", "cannot start services: %s", e.msg);
        goto out;
    }
    services_up = true;
    av_rest_set_services(g_registry, g_store, g_rpc, g_sync);

    router = av_router_create();
    if (router == NULL || av_rest_register_all(router, cfg.debug_routes, &e) != AV_OK) {
        av_log(AV_LOG_ERROR, "main", "cannot register routes: %s", router == NULL ? "out of memory" : e.msg);
        goto out;
    }

    av_http_config hc;
    memset(&hc, 0, sizeof(hc));
    hc.listen = cfg.server.listen;
    hc.max_body = (size_t) cfg.server.max_body;
    hc.workers = worker_count(&cfg);
    hc.max_connections = (size_t) cfg.server.max_connections;
    hc.request_timeout_ms = (int) cfg.server.request_timeout_ms;
    hc.idle_timeout_ms = (int) cfg.server.idle_timeout_ms;
    hc.api_keys_file = cfg.server.api_keys_file;
    hc.ratelimit_rps = cfg.server.ratelimit_rps;
    hc.ratelimit_burst = cfg.server.ratelimit_burst;
    hc.router = router;
    app.http = av_http_create(loop, &hc, &e);
    if (app.http == NULL) {
        av_log(AV_LOG_ERROR, "main", "cannot create the HTTP server: %s", e.msg);
        goto out;
    }
    av_rest_set_auth(av_http_auth(app.http));
    uv_signal_init(loop, &app.sigterm);
    uv_signal_init(loop, &app.sigint);
    app.sigterm.data = &app;
    app.sigint.data = &app;
    if (av_http_listen(app.http, &e) != AV_OK) {
        av_log(AV_LOG_ERROR, "main", "%s", e.msg);
        av_http_shutdown(app.http, http_done, &app);
        uv_run(loop, UV_RUN_DEFAULT);
        goto out;
    }
    uv_signal_start(&app.sigterm, on_signal, SIGTERM);
    uv_signal_start(&app.sigint, on_signal, SIGINT);
    av_log(AV_LOG_INFO, "http", "listening on %s (port %d), %zu workers%s", cfg.server.listen,
           av_http_port(app.http), hc.workers, cfg.debug_routes ? ", debug routes ON" : "");

    uv_run(loop, UV_RUN_DEFAULT);
    exit_code = app.http_done ? 0 : 1;
    av_log(AV_LOG_INFO, "main", "http server stopped");

out:
    if (services_up) {
        services_stop();
    }
    av_rest_set_auth(NULL);
    av_http_destroy(app.http);
    av_router_destroy(router);
    uv_loop_close(loop);
    av_log(AV_LOG_INFO, "main", "shutdown complete");
    av_config_free(&cfg);
    return exit_code;
}
