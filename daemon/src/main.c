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

/*
 * TODO(Lane D): open the LMDB store in cfg->storage.data_dir, load the registry (stored
 * collections plus cfg->collections_json), start the sync engine, call av_rest_set_sync_provider,
 * av_rest_set_ready, av_rest_set_info (writes_enabled) and av_rest_add_registrar for the routes
 * of each module. When cfg->resync is set, discard the stored cursors first.
 */
static av_status services_start(const av_config *cfg, av_err *e)
{
    (void) e;
    av_rest_set_info(cfg->arkiv.chain_id, false);
    return AV_OK;
}

/* TODO(Lane D): stop sync at a block boundary, stop the applier, close the registry and LMDB. */
static void services_stop(void)
{
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
    hc.router = router;
    app.http = av_http_create(loop, &hc, &e);
    if (app.http == NULL) {
        av_log(AV_LOG_ERROR, "main", "cannot create the HTTP server: %s", e.msg);
        goto out;
    }
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
    av_http_destroy(app.http);
    av_router_destroy(router);
    uv_loop_close(loop);
    av_log(AV_LOG_INFO, "main", "shutdown complete");
    av_config_free(&cfg);
    return exit_code;
}
