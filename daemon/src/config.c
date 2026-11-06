/*
 * config.c: TOML + environment configuration. See config.h.
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/fileio.h"
#include "toml.h"
#include "yyjson.h"

#define MiB (1024LL * 1024LL)
#define GiB (1024LL * MiB)

typedef enum { K_STR, K_INT, K_DBL, K_LIST } kind;

typedef struct {
    const char *section;
    const char *key;
    kind k;
    size_t off;
    double min; /* numeric range (K_INT, K_DBL) */
    double max;
} field;

#define F(sec, key, kind_, member, lo, hi) \
    {sec, key, kind_, offsetof(av_config, member), (double) (lo), (double) (hi)}

/* clang-format off */
static const field k_fields[] = {
    F("server", "listen", K_STR, server.listen, 0, 0),
    F("server", "threads", K_INT, server.threads, 0, 256),
    F("server", "max_body", K_INT, server.max_body, 1024, GiB),
    F("server", "max_connections", K_INT, server.max_connections, 1, 100000),
    F("server", "request_timeout_ms", K_INT, server.request_timeout_ms, 100, 600000),
    F("server", "idle_timeout_ms", K_INT, server.idle_timeout_ms, 100, 3600000),
    F("server", "api_keys_file", K_STR, server.api_keys_file, 0, 0),
    F("server", "ratelimit_rps", K_DBL, server.ratelimit_rps, 0, 100000),
    F("server", "ratelimit_burst", K_DBL, server.ratelimit_burst, 0, 100000),

    F("arkiv", "rpc_urls", K_LIST, arkiv.rpc_urls, 0, 0),
    F("arkiv", "ws_url", K_STR, arkiv.ws_url, 0, 0),
    F("arkiv", "access_key", K_STR, arkiv.access_key, 0, 0),
    F("arkiv", "chain_id", K_INT, arkiv.chain_id, 1, 9007199254740991.0),
    F("arkiv", "confirmations", K_INT, arkiv.confirmations, 0, 1000),
    F("arkiv", "max_log_range", K_INT, arkiv.max_log_range, 1, 100000),
    F("arkiv", "rpc_budget_per_sec", K_DBL, arkiv.rpc_budget_per_sec, 0.1, 100000),
    F("arkiv", "timeout_ms", K_INT, arkiv.timeout_ms, 100, 300000),

    F("storage", "data_dir", K_STR, storage.data_dir, 0, 0),
    F("storage", "map_size", K_INT, storage.map_size, 1 * MiB, 1024 * GiB),

    F("sync", "bootstrap_parallelism", K_INT, sync.bootstrap_parallelism, 1, 64),
    F("sync", "poll_interval_ms", K_INT, sync.poll_interval_ms, 50, 600000),
    F("sync", "reconcile_interval_s", K_INT, sync.reconcile_interval_s, 1, 86400),
    F("sync", "ring_size", K_INT, sync.ring_size, 1, 100000),

    F("log", "level", K_STR, log.level, 0, 0),
    F("log", "format", K_STR, log.format, 0, 0),
};
/* clang-format on */
#define N_FIELDS (sizeof(k_fields) / sizeof(k_fields[0]))

static const char *const k_sections[] = {"server", "arkiv", "storage", "sync", "log"};
static const char *const k_reserved[] = {"hnsw", "signer"};

static char *dup_s(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

static void *at(av_config *c, const field *f)
{
    return (char *) c + f->off;
}

static const void *cat(const av_config *c, const field *f)
{
    return (const char *) c + f->off;
}

static void free_list(char ***list, size_t *n)
{
    for (size_t i = 0; i < *n; i++) {
        free((*list)[i]);
    }
    free(*list);
    *list = NULL;
    *n = 0;
}

av_status av_config_set_string(char **field_ptr, const char *value, av_err *e)
{
    char *d = dup_s(value);
    if (d == NULL) {
        av_err_set(e, "internal", "config: out of memory");
        return AV_ERR;
    }
    free(*field_ptr);
    *field_ptr = d;
    return AV_OK;
}

static av_status set_list(av_config *c, char ***list, size_t *n, const char *const *items, size_t count,
                          av_err *e)
{
    (void) c;
    char **nl = calloc(count > 0 ? count : 1, sizeof(char *));
    if (nl == NULL) {
        av_err_set(e, "internal", "config: out of memory");
        return AV_ERR;
    }
    for (size_t i = 0; i < count; i++) {
        nl[i] = dup_s(items[i]);
        if (nl[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                free(nl[j]);
            }
            free(nl);
            av_err_set(e, "internal", "config: out of memory");
            return AV_ERR;
        }
    }
    free_list(list, n);
    *list = nl;
    *n = count;
    return AV_OK;
}

av_status av_config_defaults(av_config *c, av_err *e)
{
    memset(c, 0, sizeof(*c));
    c->server.threads = 0;
    c->server.max_body = 32 * MiB;
    c->server.max_connections = 1024;
    c->server.request_timeout_ms = 30000;
    c->server.idle_timeout_ms = 60000;
    c->arkiv.chain_id = 7794095;
    c->arkiv.confirmations = 0;
    c->arkiv.max_log_range = 2000;
    c->arkiv.rpc_budget_per_sec = 20;
    c->arkiv.timeout_ms = 10000;
    c->storage.map_size = 64 * GiB;
    c->sync.bootstrap_parallelism = 4;
    c->sync.poll_interval_ms = 1000;
    c->sync.reconcile_interval_s = 60;
    c->sync.ring_size = 128;
    static const char *const urls[] = {"https://rpc.rabdi.db-chain.testnet.arkiv.network"};
    if (av_config_set_string(&c->server.listen, "127.0.0.1:8080", e) != AV_OK ||
        av_config_set_string(&c->server.api_keys_file, "", e) != AV_OK ||
        av_config_set_string(&c->arkiv.ws_url, "wss://rpc.rabdi.db-chain.testnet.arkiv.network", e) !=
            AV_OK ||
        av_config_set_string(&c->arkiv.access_key, "", e) != AV_OK ||
        av_config_set_string(&c->storage.data_dir, "/var/lib/arkivector", e) != AV_OK ||
        av_config_set_string(&c->log.level, "info", e) != AV_OK ||
        av_config_set_string(&c->log.format, "text", e) != AV_OK ||
        set_list(c, &c->arkiv.rpc_urls, &c->arkiv.n_rpc_urls, urls, 1, e) != AV_OK) {
        return AV_ERR;
    }
    return AV_OK;
}

void av_config_free(av_config *c)
{
    free(c->server.listen);
    free(c->server.api_keys_file);
    free_list(&c->arkiv.rpc_urls, &c->arkiv.n_rpc_urls);
    free(c->arkiv.ws_url);
    free(c->arkiv.access_key);
    free(c->storage.data_dir);
    free(c->log.level);
    free(c->log.format);
    free_list(&c->collections_json, &c->n_collections);
    memset(c, 0, sizeof(*c));
}

/* ---- error context ---- */

typedef struct {
    const char *origin; /* file path or "<string>" */
    const char *text;   /* copy of the TOML text for line lookup, or NULL */
} ctx;

/* 1-based line of `key` inside [section] (or of the [section] header when key is NULL), 0 if unknown. */
static int find_line(const char *text, const char *section, const char *key)
{
    if (text == NULL) {
        return 0;
    }
    char cur[64] = "";
    int line = 1;
    const char *p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t) (eol - p) : strlen(p);
        const char *s = p;
        const char *end = p + len;
        while (s < end && (*s == ' ' || *s == '\t')) {
            s++;
        }
        if (s < end && *s == '[') {
            while (s < end && *s == '[') {
                s++;
            }
            const char *b = s;
            while (s < end && *s != ']') {
                s++;
            }
            size_t n = (size_t) (s - b);
            if (n >= sizeof(cur)) {
                n = sizeof(cur) - 1;
            }
            memcpy(cur, b, n);
            cur[n] = '\0';
            if (key == NULL && strcmp(cur, section) == 0) {
                return line;
            }
        } else if (key != NULL && strcmp(cur, section) == 0) {
            size_t kl = strlen(key);
            if ((size_t) (end - s) >= kl && strncmp(s, key, kl) == 0) {
                const char *q = s + kl;
                while (q < end && (*q == ' ' || *q == '\t')) {
                    q++;
                }
                if (q < end && *q == '=') {
                    return line;
                }
            }
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
        line++;
    }
    return 0;
}

static void cfg_err(av_err *e, const ctx *cx, const char *section, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void cfg_err(av_err *e, const ctx *cx, const char *section, const char *key, const char *fmt, ...)
{
    char what[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    int line = (cx != NULL && section != NULL) ? find_line(cx->text, section, key) : 0;
    char where[160];
    if (cx != NULL && cx->origin != NULL && line > 0) {
        snprintf(where, sizeof(where), "%s:%d: ", cx->origin, line);
    } else if (cx != NULL && cx->origin != NULL) {
        snprintf(where, sizeof(where), "%s: ", cx->origin);
    } else {
        where[0] = '\0';
    }
    if (section != NULL && key != NULL) {
        av_err_set(e, "invalid_argument", "config: %s[%s] %s: %s", where, section, key, what);
    } else if (section != NULL) {
        av_err_set(e, "invalid_argument", "config: %s[%s]: %s", where, section, what);
    } else {
        av_err_set(e, "invalid_argument", "config: %s%s", where, what);
    }
}

/* ---- value setters shared by TOML and env ---- */

static av_status check_range(const field *f, double v, const ctx *cx, av_err *e)
{
    if (v < f->min || v > f->max) {
        cfg_err(e, cx, f->section, f->key, "value %.15g is out of range [%.15g, %.15g]", v, f->min, f->max);
        return AV_ERR;
    }
    return AV_OK;
}

static av_status set_int(av_config *c, const field *f, int64_t v, const ctx *cx, av_err *e)
{
    if (check_range(f, (double) v, cx, e) != AV_OK) {
        return AV_ERR;
    }
    *(int64_t *) at(c, f) = v;
    return AV_OK;
}

static av_status set_dbl(av_config *c, const field *f, double v, const ctx *cx, av_err *e)
{
    if (!isfinite(v) || check_range(f, v, cx, e) != AV_OK) {
        if (!isfinite(v)) {
            cfg_err(e, cx, f->section, f->key, "value must be a finite number");
        }
        return AV_ERR;
    }
    *(double *) at(c, f) = v;
    return AV_OK;
}

/* ---- TOML ---- */

static const field *find_field(const char *section, const char *key)
{
    for (size_t i = 0; i < N_FIELDS; i++) {
        if (strcmp(k_fields[i].section, section) == 0 && strcmp(k_fields[i].key, key) == 0) {
            return &k_fields[i];
        }
    }
    return NULL;
}

static bool in_list(const char *s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static av_status load_field(av_config *c, const field *f, toml_table_t *tab, const ctx *cx, av_err *e)
{
    switch (f->k) {
    case K_STR: {
        toml_datum_t d = toml_string_in(tab, f->key);
        if (!d.ok) {
            cfg_err(e, cx, f->section, f->key, "expected a string");
            return AV_ERR;
        }
        av_status st = av_config_set_string((char **) at(c, f), d.u.s, e);
        free(d.u.s);
        return st;
    }
    case K_INT: {
        toml_datum_t d = toml_int_in(tab, f->key);
        if (!d.ok) {
            cfg_err(e, cx, f->section, f->key, "expected an integer");
            return AV_ERR;
        }
        return set_int(c, f, d.u.i, cx, e);
    }
    case K_DBL: {
        toml_datum_t d = toml_double_in(tab, f->key);
        if (d.ok) {
            return set_dbl(c, f, d.u.d, cx, e);
        }
        d = toml_int_in(tab, f->key);
        if (!d.ok) {
            cfg_err(e, cx, f->section, f->key, "expected a number");
            return AV_ERR;
        }
        return set_dbl(c, f, (double) d.u.i, cx, e);
    }
    case K_LIST: {
        toml_array_t *arr = toml_array_in(tab, f->key);
        char **items = NULL;
        size_t n = 0;
        if (arr == NULL) {
            toml_datum_t d = toml_string_in(tab, f->key);
            if (!d.ok) {
                cfg_err(e, cx, f->section, f->key, "expected an array of strings");
                return AV_ERR;
            }
            items = calloc(1, sizeof(char *));
            if (items == NULL) {
                free(d.u.s);
                av_err_set(e, "internal", "config: out of memory");
                return AV_ERR;
            }
            items[0] = d.u.s;
            n = 1;
        } else {
            int cnt = toml_array_nelem(arr);
            items = calloc((size_t) cnt + 1, sizeof(char *));
            if (items == NULL) {
                av_err_set(e, "internal", "config: out of memory");
                return AV_ERR;
            }
            for (int i = 0; i < cnt; i++) {
                toml_datum_t d = toml_string_at(arr, i);
                if (!d.ok) {
                    for (size_t j = 0; j < n; j++) {
                        free(items[j]);
                    }
                    free(items);
                    cfg_err(e, cx, f->section, f->key, "element %d is not a string", i);
                    return AV_ERR;
                }
                items[n++] = d.u.s;
            }
        }
        av_status st =
            set_list(c, (char ***) at(c, f), &c->arkiv.n_rpc_urls, (const char *const *) items, n, e);
        for (size_t j = 0; j < n; j++) {
            free(items[j]);
        }
        free(items);
        return st;
    }
    }
    return AV_ERR;
}

/* TOML value to yyjson, recursively. depth-limited. */
static yyjson_mut_val *toml_to_json(yyjson_mut_doc *doc, toml_table_t *tab, toml_array_t *arr, int depth);

static yyjson_mut_val *scalar_to_json(yyjson_mut_doc *doc, toml_datum_t s, toml_datum_t b, toml_datum_t i,
                                      toml_datum_t d)
{
    if (s.ok) {
        yyjson_mut_val *v = yyjson_mut_strcpy(doc, s.u.s);
        free(s.u.s);
        return v;
    }
    if (b.ok) {
        return yyjson_mut_bool(doc, b.u.b != 0);
    }
    if (i.ok) {
        return yyjson_mut_sint(doc, i.u.i);
    }
    if (d.ok) {
        return yyjson_mut_real(doc, d.u.d);
    }
    return NULL;
}

static yyjson_mut_val *toml_to_json(yyjson_mut_doc *doc, toml_table_t *tab, toml_array_t *arr, int depth)
{
    if (depth > 16) {
        return NULL;
    }
    if (tab != NULL) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        for (int i = 0;; i++) {
            const char *key = toml_key_in(tab, i);
            if (key == NULL) {
                break;
            }
            yyjson_mut_val *val = NULL;
            toml_table_t *sub = toml_table_in(tab, key);
            toml_array_t *sa = sub == NULL ? toml_array_in(tab, key) : NULL;
            if (sub != NULL) {
                val = toml_to_json(doc, sub, NULL, depth + 1);
            } else if (sa != NULL) {
                val = toml_to_json(doc, NULL, sa, depth + 1);
            } else {
                val = scalar_to_json(doc, toml_string_in(tab, key), toml_bool_in(tab, key),
                                     toml_int_in(tab, key), toml_double_in(tab, key));
            }
            if (val == NULL || !yyjson_mut_obj_add_val(doc, obj, key, val)) {
                return NULL;
            }
        }
        return obj;
    }
    yyjson_mut_val *out = yyjson_mut_arr(doc);
    int n = toml_array_nelem(arr);
    for (int i = 0; i < n; i++) {
        yyjson_mut_val *val = NULL;
        toml_table_t *sub = toml_table_at(arr, i);
        toml_array_t *sa = sub == NULL ? toml_array_at(arr, i) : NULL;
        if (sub != NULL) {
            val = toml_to_json(doc, sub, NULL, depth + 1);
        } else if (sa != NULL) {
            val = toml_to_json(doc, NULL, sa, depth + 1);
        } else {
            val = scalar_to_json(doc, toml_string_at(arr, i), toml_bool_at(arr, i), toml_int_at(arr, i),
                                 toml_double_at(arr, i));
        }
        if (val == NULL || !yyjson_mut_arr_append(out, val)) {
            return NULL;
        }
    }
    return out;
}

static av_status load_collections(av_config *c, toml_table_t *root, const ctx *cx, av_err *e)
{
    toml_array_t *arr = toml_array_in(root, "collections");
    if (arr == NULL) {
        if (toml_table_in(root, "collections") != NULL) {
            cfg_err(e, cx, "collections", NULL, "must be an array of tables: use [[collections]]");
            return AV_ERR;
        }
        return AV_OK;
    }
    int n = toml_array_nelem(arr);
    for (int i = 0; i < n; i++) {
        toml_table_t *t = toml_table_at(arr, i);
        if (t == NULL) {
            cfg_err(e, cx, "collections", NULL, "entry %d is not a table", i);
            return AV_ERR;
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *obj = doc != NULL ? toml_to_json(doc, t, NULL, 0) : NULL;
        if (obj == NULL) {
            yyjson_mut_doc_free(doc);
            cfg_err(e, cx, "collections", NULL,
                    "entry %d cannot be converted to JSON (unsupported value type?)", i);
            return AV_ERR;
        }
        yyjson_mut_doc_set_root(doc, obj);
        yyjson_mut_val *name = yyjson_mut_obj_get(obj, "name");
        if (!yyjson_mut_is_str(name)) {
            yyjson_mut_doc_free(doc);
            cfg_err(e, cx, "collections", NULL, "entry %d needs a string 'name'", i);
            return AV_ERR;
        }
        char *json = yyjson_mut_write(doc, 0, NULL);
        yyjson_mut_doc_free(doc);
        char **nl =
            json != NULL ? realloc(c->collections_json, (c->n_collections + 1) * sizeof(char *)) : NULL;
        if (nl == NULL) {
            free(json);
            av_err_set(e, "internal", "config: out of memory");
            return AV_ERR;
        }
        c->collections_json = nl;
        c->collections_json[c->n_collections++] = json;
    }
    return AV_OK;
}

static av_status load_table(av_config *c, toml_table_t *root, const ctx *cx, av_err *e)
{
    /* Top level: only known sections. */
    for (int i = 0;; i++) {
        const char *key = toml_key_in(root, i);
        if (key == NULL) {
            break;
        }
        if (in_list(key, k_sections, sizeof(k_sections) / sizeof(k_sections[0])) ||
            strcmp(key, "collections") == 0) {
            continue;
        }
        if (in_list(key, k_reserved, sizeof(k_reserved) / sizeof(k_reserved[0]))) {
            av_log(AV_LOG_WARN, "config", "section [%s] is not used by this build yet and is ignored", key);
            continue;
        }
        cfg_err(e, cx, key, NULL, "unknown section or top-level key '%s'", key);
        return AV_ERR;
    }
    for (size_t s = 0; s < sizeof(k_sections) / sizeof(k_sections[0]); s++) {
        toml_table_t *tab = toml_table_in(root, k_sections[s]);
        if (tab == NULL) {
            if (toml_key_exists(root, k_sections[s])) {
                cfg_err(e, cx, NULL, NULL, "'%s' must be a table", k_sections[s]);
                return AV_ERR;
            }
            continue;
        }
        for (int i = 0;; i++) {
            const char *key = toml_key_in(tab, i);
            if (key == NULL) {
                break;
            }
            const field *f = find_field(k_sections[s], key);
            if (f == NULL) {
                cfg_err(e, cx, k_sections[s], key, "unknown key");
                return AV_ERR;
            }
            if (load_field(c, f, tab, cx, e) != AV_OK) {
                return AV_ERR;
            }
        }
    }
    return load_collections(c, root, cx, e);
}

av_status av_config_load_string(av_config *c, const char *toml_text, const char *origin, av_err *e)
{
    char *work = dup_s(toml_text);
    char *scan = dup_s(toml_text);
    if (work == NULL || scan == NULL) {
        free(work);
        free(scan);
        av_err_set(e, "internal", "config: out of memory");
        return AV_ERR;
    }
    ctx cx = {origin != NULL ? origin : "<string>", scan};
    char errbuf[200];
    toml_table_t *root = toml_parse(work, errbuf, (int) sizeof(errbuf));
    av_status st = AV_ERR;
    if (root == NULL) {
        av_err_set(e, "invalid_argument", "config: %s: %s", cx.origin, errbuf);
    } else {
        st = load_table(c, root, &cx, e);
        toml_free(root);
    }
    free(work);
    free(scan);
    if (st != AV_OK) {
        return AV_ERR;
    }
    return AV_OK;
}

static av_status read_file(const char *path, char **out, av_err *e)
{
    size_t len = 0;
    char *buf = av_read_file(path, "config", (size_t) 16 * 1024 * 1024, &len, e);
    if (buf == NULL) {
        return AV_ERR;
    }
    if (strlen(buf) != len) {
        free(buf);
        av_err_set(e, "invalid_argument", "config: %s contains a NUL byte", path);
        return AV_ERR;
    }
    *out = buf;
    return AV_OK;
}

/* ---- environment ---- */

static void env_name(char *out, size_t cap, const field *f)
{
    snprintf(out, cap, "ARKIVECTOR_%s_%s", f->section, f->key);
    for (char *p = out; *p != '\0'; p++) {
        *p = (char) toupper((unsigned char) *p);
    }
}

static av_status env_int(const char *name, const char *val, int64_t *out, av_err *e)
{
    char *end = NULL;
    errno = 0;
    long long v = strtoll(val, &end, 10);
    if (errno != 0 || end == val || *end != '\0') {
        av_err_set(e, "invalid_argument", "config: environment %s: '%s' is not an integer", name, val);
        return AV_ERR;
    }
    *out = v;
    return AV_OK;
}

av_status av_config_apply_env(av_config *c, av_err *e)
{
    for (size_t i = 0; i < N_FIELDS; i++) {
        const field *f = &k_fields[i];
        char name[96];
        env_name(name, sizeof(name), f);
        const char *val = getenv(name);
        if (val == NULL) {
            continue;
        }
        ctx cx = {name, NULL};
        switch (f->k) {
        case K_STR:
            if (av_config_set_string((char **) at(c, f), val, e) != AV_OK) {
                return AV_ERR;
            }
            break;
        case K_INT: {
            int64_t v;
            if (env_int(name, val, &v, e) != AV_OK || set_int(c, f, v, &cx, e) != AV_OK) {
                return AV_ERR;
            }
            break;
        }
        case K_DBL: {
            char *end = NULL;
            double v = strtod(val, &end);
            if (end == val || *end != '\0') {
                av_err_set(e, "invalid_argument", "config: environment %s: '%s' is not a number", name, val);
                return AV_ERR;
            }
            if (set_dbl(c, f, v, &cx, e) != AV_OK) {
                return AV_ERR;
            }
            break;
        }
        case K_LIST: {
            char *copy = dup_s(val);
            if (copy == NULL) {
                av_err_set(e, "internal", "config: out of memory");
                return AV_ERR;
            }
            const char *items[64];
            size_t n = 0;
            for (char *tok = copy; tok != NULL && n < 64;) {
                char *comma = strchr(tok, ',');
                if (comma != NULL) {
                    *comma = '\0';
                }
                while (*tok == ' ') {
                    tok++;
                }
                if (*tok != '\0') {
                    items[n++] = tok;
                }
                tok = comma != NULL ? comma + 1 : NULL;
            }
            av_status st = set_list(c, (char ***) at(c, f), &c->arkiv.n_rpc_urls, items, n, e);
            free(copy);
            if (st != AV_OK) {
                return AV_ERR;
            }
            break;
        }
        }
    }
    return AV_OK;
}

/* ---- validation ---- */

static bool has_prefix(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

av_status av_config_validate(const av_config *c, av_err *e)
{
    for (size_t i = 0; i < N_FIELDS; i++) {
        const field *f = &k_fields[i];
        if (f->k == K_INT && check_range(f, (double) *(const int64_t *) cat(c, f), NULL, e) != AV_OK) {
            return AV_ERR;
        }
        if (f->k == K_DBL && check_range(f, *(const double *) cat(c, f), NULL, e) != AV_OK) {
            return AV_ERR;
        }
    }
    const char *colon = strrchr(c->server.listen, ':');
    if (colon == NULL || colon == c->server.listen || colon[1] == '\0') {
        cfg_err(e, NULL, "server", "listen", "'%s' is not host:port", c->server.listen);
        return AV_ERR;
    }
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (*end != '\0' || port < 0 || port > 65535) {
        cfg_err(e, NULL, "server", "listen", "'%s' has an invalid port", c->server.listen);
        return AV_ERR;
    }
    if (c->storage.data_dir[0] == '\0') {
        cfg_err(e, NULL, "storage", "data_dir", "must not be empty");
        return AV_ERR;
    }
    av_log_level lvl;
    av_log_format fmt;
    if (!av_log_parse_level(c->log.level, &lvl)) {
        cfg_err(e, NULL, "log", "level", "'%s' is not one of debug, info, warn, error", c->log.level);
        return AV_ERR;
    }
    if (!av_log_parse_format(c->log.format, &fmt)) {
        cfg_err(e, NULL, "log", "format", "'%s' is not one of text, json", c->log.format);
        return AV_ERR;
    }
    if (c->arkiv.n_rpc_urls == 0) {
        cfg_err(e, NULL, "arkiv", "rpc_urls", "needs at least one URL");
        return AV_ERR;
    }
    for (size_t i = 0; i < c->arkiv.n_rpc_urls; i++) {
        if (!has_prefix(c->arkiv.rpc_urls[i], "http://") && !has_prefix(c->arkiv.rpc_urls[i], "https://")) {
            cfg_err(e, NULL, "arkiv", "rpc_urls", "'%s' must start with http:// or https://",
                    c->arkiv.rpc_urls[i]);
            return AV_ERR;
        }
    }
    if (c->arkiv.ws_url[0] != '\0' && !has_prefix(c->arkiv.ws_url, "ws://") &&
        !has_prefix(c->arkiv.ws_url, "wss://")) {
        cfg_err(e, NULL, "arkiv", "ws_url", "'%s' must start with ws:// or wss:// (or be empty)",
                c->arkiv.ws_url);
        return AV_ERR;
    }
    for (size_t i = 0; i < c->n_collections; i++) {
        yyjson_doc *d = yyjson_read(c->collections_json[i], strlen(c->collections_json[i]), 0);
        const char *name = d != NULL ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d), "name")) : NULL;
        for (size_t j = 0; name != NULL && j < i; j++) {
            yyjson_doc *d2 = yyjson_read(c->collections_json[j], strlen(c->collections_json[j]), 0);
            const char *n2 =
                d2 != NULL ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d2), "name")) : NULL;
            bool dup = n2 != NULL && strcmp(n2, name) == 0;
            yyjson_doc_free(d2);
            if (dup) {
                cfg_err(e, NULL, "collections", NULL, "duplicate collection name '%s'", name);
                yyjson_doc_free(d);
                return AV_ERR;
            }
        }
        yyjson_doc_free(d);
    }
    return AV_OK;
}

av_status av_config_load(av_config *c, const char *path, av_err *e)
{
    if (av_config_defaults(c, e) != AV_OK) {
        return AV_ERR;
    }
    if (path != NULL) {
        char *text = NULL;
        if (read_file(path, &text, e) != AV_OK) {
            return AV_ERR;
        }
        av_status st = av_config_load_string(c, text, path, e);
        free(text);
        if (st != AV_OK) {
            return AV_ERR;
        }
    }
    if (av_config_apply_env(c, e) != AV_OK) {
        return AV_ERR;
    }
    return av_config_validate(c, e);
}

void av_config_log_summary(const av_config *c)
{
    av_log(AV_LOG_INFO, "config", "listen=%s threads=%lld max_body=%lld data_dir=%s map_size=%lld log=%s/%s",
           c->server.listen, (long long) c->server.threads, (long long) c->server.max_body,
           c->storage.data_dir, (long long) c->storage.map_size, c->log.level, c->log.format);
    av_log(AV_LOG_INFO, "config", "arkiv rpc_urls=%zu ws=%s chain_id=%lld credentials=%s confirmations=%lld",
           c->arkiv.n_rpc_urls, c->arkiv.ws_url[0] != '\0' ? c->arkiv.ws_url : "(none)",
           (long long) c->arkiv.chain_id, c->arkiv.access_key[0] != '\0' ? "configured" : "none",
           (long long) c->arkiv.confirmations);
    av_log(AV_LOG_INFO, "config", "collections=%zu", c->n_collections);
}
