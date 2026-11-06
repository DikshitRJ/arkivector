/*
 * test_rpc_mock.c: suite arkiv_rpc. End-to-end tests of the Arkiv client against daemon/testkit/mock-arkiv.
 *
 * The suite spawns `$AV_PYTHON server.py --port 0 --seed 7` (default python
 * /tmp/claude-1000/itvenv/bin/python, which needs aiohttp + pycryptodome), reads "READY <port>",
 * drives the mock's control API with libcurl and kills it at the end. Every test SKIPs when the
 * mock cannot start. (Not in the SDK or production path; test code only.)
 */
#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arkiv/rpc.h"
#include "arkiv/ws.h"
#include "greatest.h"
#include "yyjson.h"

#define NENT 7

static pid_t g_pid = -1;
static int g_port;
static bool g_ready;
static char g_base[64]; /* http://127.0.0.1:PORT */
static uint8_t g_keys[NENT][32];
static uint64_t g_head;

/* ------------------------------------------------------------------ mock process */

static bool spawn_mock(void)
{
    int pfd[2];
    const char *py = getenv("AV_PYTHON");
    char script[1024];
    if (py == NULL)
        py = "/tmp/claude-1000/itvenv/bin/python";
    snprintf(script, sizeof script, "%s/daemon/testkit/mock-arkiv/server.py", AV_REPO_ROOT);
    if (access(py, X_OK) != 0 || access(script, R_OK) != 0 || pipe(pfd) != 0)
        return false;
    g_pid = fork();
    if (g_pid < 0)
        return false;
    if (g_pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl(py, py, script, "--port", "0", "--seed", "7", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char line[128];
    size_t n = 0;
    for (int waited = 0; waited < 15000 && n < sizeof line - 1;) {
        struct pollfd p = {.fd = pfd[0], .events = POLLIN, .revents = 0};
        int pr = poll(&p, 1, 200);
        if (pr == 0) {
            waited += 200;
            continue;
        }
        char ch;
        if (pr < 0 || read(pfd[0], &ch, 1) != 1)
            break;
        if (ch == '\n')
            break;
        line[n++] = ch;
    }
    line[n] = '\0';
    close(pfd[0]);
    if (sscanf(line, "READY %d", &g_port) != 1) {
        kill(g_pid, SIGKILL);
        waitpid(g_pid, NULL, 0);
        g_pid = -1;
        return false;
    }
    snprintf(g_base, sizeof g_base, "http://127.0.0.1:%d", g_port);
    return true;
}

static void stop_mock(void)
{
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        waitpid(g_pid, NULL, 0);
        g_pid = -1;
    }
}

/* ------------------------------------------------------------------ control API client */

typedef struct {
    char buf[1 << 16];
    size_t len;
} cbuf;

static size_t cb_write(char *p, size_t s, size_t m, void *ud)
{
    cbuf *b = (cbuf *)ud;
    size_t n = s * m;
    if (b->len + n >= sizeof b->buf)
        return 0;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return n;
}

/* Returns the HTTP status (0 on transport error); response body in out. */
static long ctl(const char *method, const char *path, const char *body, cbuf *out)
{
    char url[256];
    CURL *c = curl_easy_init();
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    long status = 0;
    out->len = 0;
    out->buf[0] = '\0';
    snprintf(url, sizeof url, "%s/_mock%s", g_base, path);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (body != NULL)
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return status;
}

static av_rpc *mk_rpc(int retries, const char *key)
{
    char url[96];
    static char u[96];
    const char *urls[1];
    av_err e;
    snprintf(url, sizeof url, "%s/", g_base);
    memcpy(u, url, sizeof u);
    urls[0] = u;
    av_rpc_config cfg = {.urls = urls,
                         .n_urls = 1,
                         .access_key = key,
                         .timeout_ms = 5000,
                         .max_retries = retries,
                         .budget_per_sec = 0,
                         .chain_id = 0x76edaf};
    return av_rpc_create(&cfg, &e);
}

#define NEED_MOCK()                                                                                                    \
    do {                                                                                                               \
        if (!g_ready)                                                                                                  \
            SKIPm("mock-arkiv could not start (set AV_PYTHON)");                                                       \
    } while (0)

static int key_index(const uint8_t k[32])
{
    for (int i = 0; i < NENT; i++)
        if (memcmp(g_keys[i], k, 32) == 0)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ tests */

TEST seed_chain(void)
{
    cbuf out;
    char body[4096];
    size_t w = 0;
    NEED_MOCK();
    w += (size_t)snprintf(body + w, sizeof body - w, "[");
    for (int i = 0; i < NENT; i++)
        w += (size_t)snprintf(body + w, sizeof body - w,
                              "%s{\"min_lifetime\":100,\"attributes\":[{\"name\":\"arkiv.schema\",\"type\":\"str\","
                              "\"value\":\"%s\"},{\"name\":\"score\",\"type\":\"dec\",\"value\":\"%d.5\"},"
                              "{\"name\":\"av.vec\",\"type\":\"bytes\",\"value\":\"0x0%dff\"}]}",
                              i ? "," : "", i < 4 ? "t.even" : "t.odd", i, i);
    snprintf(body + w, sizeof body - w, "]");
    ASSERT_EQ(200, ctl("POST", "/entities", body, &out));
    yyjson_doc *d = yyjson_read(out.buf, out.len, 0);
    ASSERT(d != NULL);
    yyjson_val *created = yyjson_obj_get(yyjson_doc_get_root(d), "created");
    ASSERT_EQ(NENT, yyjson_arr_size(created));
    for (size_t i = 0; i < NENT; i++) {
        const char *k = yyjson_get_str(yyjson_arr_get_first(yyjson_obj_get(yyjson_arr_get(created, i), "keys")));
        ASSERT(k != NULL && av_key_from_hex(k, g_keys[i]));
    }
    yyjson_doc_free(d);
    ASSERT_EQ(200, ctl("POST", "/mine", "{\"n\":1}", &out));
    PASS();
}

TEST block_calls(void)
{
    NEED_MOCK();
    av_rpc *r = mk_rpc(0, NULL);
    av_err e;
    ASSERT(r != NULL);
    ASSERT_EQ(AV_OK, av_rpc_block_number(r, &g_head, &e));
    ASSERT(g_head >= 1);
    av_block b, p;
    ASSERT_EQ(AV_OK, av_rpc_get_block(r, g_head, &b, &e));
    ASSERT_EQ(g_head, b.number);
    ASSERT_EQ(AV_OK, av_rpc_get_block(r, g_head - 1, &p, &e));
    ASSERT(memcmp(b.parent_hash, p.hash, 32) == 0);
    ASSERT(b.timestamp > 0);
    ASSERT_EQ(AV_ERR, av_rpc_get_block(r, g_head + 1000, &b, &e));
    ASSERT_STR_EQ("arkiv_rpc_error", e.code);
    av_block_timing t;
    ASSERT_EQ(AV_OK, av_rpc_block_timing(r, &t, &e));
    ASSERT_EQ(g_head, t.current_block);
    ASSERT(t.block_duration > 0);
    av_rpc_destroy(r);
    PASS();
}

TEST query_pagination(void)
{
    NEED_MOCK();
    av_rpc *r = mk_rpc(0, NULL);
    av_arena *ar = av_arena_create("q", 1 << 16, 0);
    av_err e;
    int seen[NENT] = {0};
    size_t total = 0;
    size_t pages = 0;
    char *cursor = NULL;
    uint8_t last[32] = {0};
    ASSERT(r != NULL);
    do {
        av_query_page pg;
        int code;
        ASSERT_EQ(AV_OK, av_rpc_query(r, NULL, cursor, 3, false, ar, &pg, &code, &e));
        pages++;
        for (size_t i = 0; i < pg.n; i++) {
            int ix = key_index(pg.ents[i].key);
            ASSERT(ix >= 0);
            seen[ix]++;
            ASSERT(memcmp(pg.ents[i].key, last, 32) > 0);
            memcpy(last, pg.ents[i].key, 32);
            ASSERT(pg.ents[i].expires_at > pg.block);
            ASSERT(av_entity_attr(&pg.ents[i], "av.vec") != NULL);
            ASSERT(av_entity_attr(&pg.ents[i], AV_ATTR_SCHEMA) != NULL);
        }
        total += pg.n;
        cursor = pg.next_cursor;
    } while (cursor != NULL && pages < 10);
    ASSERT_EQ(NENT, total);
    ASSERT_EQ(3, pages); /* 3 + 3 + 1 */
    for (int i = 0; i < NENT; i++)
        ASSERT_EQ(1, seen[i]);

    /* filtered base query, key_only projection */
    av_query_page pg;
    int code;
    ASSERT_EQ(AV_OK, av_rpc_query(r, "arkiv.schema = str('t.odd')", NULL, 100, true, ar, &pg, &code, &e));
    ASSERT_EQ(3, pg.n);
    ASSERT(pg.next_cursor == NULL);
    ASSERT_EQ(0, pg.ents[0].n_attrs);

    /* bad inputs are rejected locally */
    ASSERT_EQ(AV_ERR, av_rpc_query(r, NULL, NULL, 0, false, ar, &pg, &code, &e));
    ASSERT_EQ(AV_ERR, av_rpc_query(r, NULL, "0x12", 10, false, ar, &pg, &code, &e));
    ASSERT_STR_EQ("invalid_argument", e.code);
    av_arena_destroy(ar);
    av_rpc_destroy(r);
    PASS();
}

TEST get_entity_cases(void)
{
    NEED_MOCK();
    av_rpc *r = mk_rpc(0, NULL);
    av_arena *ar = av_arena_create("g", 1 << 14, 0);
    av_err e;
    av_entity ent;
    bool found = false;
    ASSERT(r != NULL);
    ASSERT_EQ(AV_OK, av_rpc_get_entity(r, g_keys[2], ar, &ent, &found, &e));
    ASSERT(found);
    ASSERT(memcmp(ent.key, g_keys[2], 32) == 0);
    const av_attr *sc = av_entity_attr(&ent, "score");
    ASSERT(sc != NULL && sc->type == AV_AT_DEC && sc->len == 32);
    {
        static const uint8_t want[8] = {0x22, 0xb1, 0xc8, 0xc1, 0x22, 0x7a, 0x00, 0x00}; /* 2.5e18 */
        for (int i = 0; i < 24; i++)
            ASSERT_EQ(0, sc->val[i]);
        ASSERT(memcmp(sc->val + 24, want, 8) == 0);
    }
    uint8_t absent[32];
    memset(absent, 0x5a, 32);
    ASSERT_EQ(AV_OK, av_rpc_get_entity(r, absent, ar, &ent, &found, &e));
    ASSERT(!found);
    av_arena_destroy(ar);
    av_rpc_destroy(r);
    PASS();
}

TEST logs_by_kind(void)
{
    NEED_MOCK();
    av_rpc *r = mk_rpc(0, NULL);
    av_arena *ar = av_arena_create("l", 1 << 16, 0);
    av_err e;
    cbuf out;
    char path[256];
    char body[256];
    char hex[67];
    av_event *evs;
    size_t n;
    int code;
    ASSERT(r != NULL);
    ASSERT_EQ(AV_OK, av_rpc_get_logs(r, 0, g_head, NULL, ar, &evs, &n, &code, &e));
    ASSERT_EQ(NENT, n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(AV_EV_CREATED, evs[i].kind);
        ASSERT(key_index(evs[i].key) >= 0);
        ASSERT(!evs[i].removed && evs[i].block >= 1 && evs[i].expires_at > evs[i].block);
    }

    /* one of each other kind, then mine */
    av_key_to_hex(g_keys[0], hex);
    snprintf(path, sizeof path, "/entities/%s", hex);
    ASSERT_EQ(200, ctl("PATCH", path, "{\"attributes\":[{\"name\":\"score\",\"type\":\"dec\",\"value\":\"9\"}]}", &out));
    av_key_to_hex(g_keys[1], hex);
    snprintf(path, sizeof path, "/entities/%s/extend", hex);
    ASSERT_EQ(200, ctl("POST", path, "{\"min_lifetime\":500}", &out));
    av_key_to_hex(g_keys[2], hex);
    snprintf(path, sizeof path, "/entities/%s/transfer", hex);
    snprintf(body, sizeof body, "{\"new_owner\":\"0x2222222222222222222222222222222222222222\"}");
    ASSERT_EQ(200, ctl("POST", path, body, &out));
    av_key_to_hex(g_keys[3], hex);
    snprintf(path, sizeof path, "/entities/%s", hex);
    ASSERT_EQ(200, ctl("DELETE", path, NULL, &out));
    ASSERT_EQ(200, ctl("POST", "/mine", "{\"n\":1}", &out));
    uint64_t head;
    ASSERT_EQ(AV_OK, av_rpc_block_number(r, &head, &e));
    ASSERT_EQ(g_head + 1, head);

    ASSERT_EQ(AV_OK, av_rpc_get_logs(r, head, head, NULL, ar, &evs, &n, &code, &e));
    ASSERT_EQ(4, n);
    ASSERT_EQ(AV_EV_PATCHED, evs[0].kind);
    ASSERT(memcmp(evs[0].key, g_keys[0], 32) == 0);
    ASSERT_EQ(AV_EV_EXTENDED, evs[1].kind);
    ASSERT(evs[1].expires_at > head);
    ASSERT_EQ(AV_EV_TRANSFERRED, evs[2].kind);
    ASSERT(evs[2].new_owner[0] == 0x22 && evs[2].prev_owner[0] == 0x11);
    ASSERT_EQ(AV_EV_DELETED, evs[3].kind);
    ASSERT(evs[3].log_index < 100);

    /* topics filter: only deletions */
    char topics[128];
    av_key_to_hex(av_event_topic0(AV_EV_DELETED), hex);
    snprintf(topics, sizeof topics, "[\"%s\"]", hex);
    ASSERT_EQ(AV_OK, av_rpc_get_logs(r, 0, head, topics, ar, &evs, &n, &code, &e));
    ASSERT_EQ(1, n);
    ASSERT(memcmp(evs[0].key, g_keys[3], 32) == 0);
    ASSERT_EQ(AV_ERR, av_rpc_get_logs(r, 5, 4, NULL, ar, &evs, &n, &code, &e));
    ASSERT_EQ(AV_ERR, av_rpc_get_logs(r, 0, 1, "{}", ar, &evs, &n, &code, &e));

    /* range limit surfaces as -32005, not retried */
    av_rpc *r3 = mk_rpc(3, NULL);
    uint64_t before = av_rpc_attempts(r3);
    ASSERT_EQ(AV_ERR, av_rpc_get_logs(r3, 0, 100000, NULL, ar, &evs, &n, &code, &e));
    ASSERT_EQ(-32005, code);
    ASSERT_EQ(1, av_rpc_attempts(r3) - before);
    av_rpc_destroy(r3);
    g_head = head;
    av_arena_destroy(ar);
    av_rpc_destroy(r);
    PASS();
}

TEST rpc_error_not_retried(void)
{
    NEED_MOCK();
    av_rpc *r = mk_rpc(3, NULL);
    av_err e;
    yyjson_doc *d = NULL;
    yyjson_val *res = NULL;
    int code = 0;
    ASSERT(r != NULL);
    ASSERT_EQ(AV_ERR, av_rpc_call(r, "arkiv_getEntity", "[\"0x12\"]", &d, &res, &code, &e));
    ASSERT_EQ(-32602, code);
    ASSERT_STR_EQ("arkiv_rpc_error", e.code);
    ASSERT(d == NULL);
    ASSERT_EQ(1, av_rpc_attempts(r));
    /* literal/limits errors from the query engine are never retried either */
    av_arena *ar = av_arena_create("e", 4096, 0);
    av_query_page pg;
    ASSERT_EQ(AV_ERR, av_rpc_query(r, "nonsense ((", NULL, 10, true, ar, &pg, &code, &e));
    ASSERT(code == -32001 || code == -32002 || code == -32004);
    ASSERT_EQ(2, av_rpc_attempts(r));
    av_arena_destroy(ar);
    av_rpc_destroy(r);
    PASS();
}

TEST faults_429_retried(void)
{
    NEED_MOCK();
    cbuf out;
    av_err e;
    uint64_t head = 0;
    av_rpc *r = mk_rpc(2, "SECRET-KEY-123");
    ASSERT(r != NULL);
    ASSERT_EQ(200, ctl("POST", "/faults", "{\"http_429_ratio\":1.0}", &out));
    ASSERT_EQ(AV_ERR, av_rpc_block_number(r, &head, &e));
    ASSERT_STR_EQ("arkiv_rpc_error", e.code);
    ASSERT(strstr(e.msg, "429") != NULL);
    ASSERT(strstr(e.msg, "SECRET-KEY-123") == NULL);
    ASSERT_EQ(3, av_rpc_attempts(r)); /* first try + 2 retries */
    ASSERT_EQ(200, ctl("POST", "/faults", "{\"http_429_ratio\":0}", &out));
    ASSERT_EQ(AV_OK, av_rpc_block_number(r, &head, &e));
    ASSERT_EQ(g_head, head);
    av_rpc_destroy(r);
    PASS();
}

TEST ws_new_head(void)
{
    NEED_MOCK();
    cbuf out;
    av_err e;
    uint64_t head = 0;
    av_ws *w = av_ws_open(g_base, "k", &e);
    ASSERT(w != NULL);
    ASSERT(av_ws_connected(w));
    ASSERT_EQ(0, av_ws_wait(w, 300, &head)); /* quiet chain: timeout */
    ASSERT_EQ(200, ctl("POST", "/mine", "{\"n\":1}", &out));
    ASSERT_EQ(1, av_ws_wait(w, 5000, &head));
    ASSERT_EQ(g_head + 1, head);
    av_ws_close(w);

    /* unreachable endpoint: not an error at open, wait reports -1 */
    av_ws *dead = av_ws_open("ws://127.0.0.1:1", NULL, &e);
    ASSERT(dead != NULL);
    ASSERT(!av_ws_connected(dead));
    ASSERT_EQ(-1, av_ws_wait(dead, 50, &head));
    av_ws_close(dead);
    ASSERT(av_ws_open("ftp://x", NULL, &e) == NULL);
    PASS();
}

TEST config_validation(void)
{
    av_err e;
    const char *bad[] = {"ftp://x"};
    av_rpc_config cfg = {.urls = bad, .n_urls = 1};
    ASSERT(av_rpc_create(&cfg, &e) == NULL);
    ASSERT_STR_EQ("invalid_argument", e.code);
    cfg.n_urls = 0;
    ASSERT(av_rpc_create(&cfg, &e) == NULL);
    PASS();
}

SUITE(arkiv_rpc)
{
    g_ready = spawn_mock();
    RUN_TEST(config_validation);
    RUN_TEST(seed_chain);
    RUN_TEST(block_calls);
    RUN_TEST(query_pagination);
    RUN_TEST(get_entity_cases);
    RUN_TEST(logs_by_kind);
    RUN_TEST(rpc_error_not_retried);
    RUN_TEST(faults_429_retried);
    RUN_TEST(ws_new_head);
    stop_mock();
}
