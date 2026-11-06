/*
 * test_rest.c: unit tests for REST endpoints (collections, entities, search, debug).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"

#include "core/arena.h"
#include "core/coldef.h"
#include "core/collection.h"
#include "core/registry.h"
#include "server/http.h"
#include "server/json_io.h"
#include "server/rest.h"
#include "server/router.h"
#include "store/lmdb_store.h"
#include "yyjson.h"

static char g_tmpdir[256];
static av_store *g_store;
static av_registry *g_reg;
static av_router *g_router;
static av_arena *g_arena;

static void setup_rest(void *udata)
{
    (void) udata;
    g_arena = av_arena_create("test_rest", 1024 * 1024, 0);

    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/av_rest_test_XXXXXX");
    if (mkdtemp(g_tmpdir) == NULL) {
        perror("mkdtemp");
        return;
    }

    av_err e;
    av_err_clear(&e);
    if (av_store_open(g_tmpdir, 64 * 1024 * 1024, &g_store, &e) != AV_OK) {
        fprintf(stderr, "av_store_open failed: %s\n", e.msg);
        return;
    }

    g_reg = av_registry_create(g_store, g_tmpdir, &e);
    if (g_reg == NULL) {
        fprintf(stderr, "av_registry_create failed: %s\n", e.msg);
        return;
    }

    av_rest_set_services(g_reg, g_store, NULL, NULL);

    g_router = av_router_create();
    if (av_rest_register_all(g_router, true, &e) != AV_OK) {
        fprintf(stderr, "av_rest_register_all failed: %s\n", e.msg);
        return;
    }
}

static void teardown_rest(void *udata)
{
    (void) udata;
    if (g_router != NULL) {
        av_router_destroy(g_router);
        g_router = NULL;
    }
    if (g_reg != NULL) {
        av_registry_destroy(g_reg);
        g_reg = NULL;
    }
    if (g_store != NULL) {
        av_store_close(g_store);
        g_store = NULL;
    }
    if (g_arena != NULL) {
        av_arena_destroy(g_arena);
        g_arena = NULL;
    }

    /* rm -rf g_tmpdir */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
    (void) system(cmd);
}

static void dispatch_req(const char *method, const char *path, const char *query,
                         const char *body, av_http_resp *resp)
{
    memset(resp, 0, sizeof(*resp));
    av_arena_reset(g_arena);

    av_http_req req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.query = query != NULL ? query : "";
    req.body = body != NULL ? body : "";
    req.body_len = body != NULL ? strlen(body) : 0;
    req.arena = g_arena;
    req.request_id = "01TEST00000000000000000000";

    av_route_match m;
    memset(&m, 0, sizeof(m));
    av_router_match(g_router, method, path, g_arena, &m);

    if (m.status != 0) {
        resp->status = m.status;
        return;
    }

    m.handler(&req, m.params, m.n_params, resp, m.ctx);
}

TEST test_collections_crud(void)
{
    av_http_resp resp;

    /* 1. GET /v1/collections (initially empty) */
    dispatch_req("GET", "/v1/collections", NULL, NULL, &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "{\"collections\":[]}") != NULL);
    free(resp.body);

    /* 2. POST /v1/collections with publish_manifest=true -> 501 */
    const char *create_pm =
        "{"
        "  \"name\": \"demo.pm\","
        "  \"column\": {\"type\": \"halfvec\", \"dimensions\": 4},"
        "  \"index\": {\"method\": \"hnsw\", \"opclass\": \"halfvec_cosine_ops\"},"
        "  \"writers\": [\"0x1111111111111111111111111111111111111111\"],"
        "  \"publish_manifest\": true"
        "}";
    dispatch_req("POST", "/v1/collections", NULL, create_pm, &resp);
    ASSERT_EQ(501, resp.status);
    ASSERT(strstr(resp.body, "publish_manifest is supported in Phase D5") != NULL);
    free(resp.body);

    /* 3. POST /v1/collections valid -> 201 */
    const char *create_valid =
        "{"
        "  \"name\": \"demo.docs\","
        "  \"column\": {\"type\": \"halfvec\", \"dimensions\": 4},"
        "  \"index\": {\"method\": \"hnsw\", \"opclass\": \"halfvec_cosine_ops\", \"with\": {\"m\": 16, \"ef_construction\": 64}},"
        "  \"writers\": [\"0x1111111111111111111111111111111111111111\"],"
        "  \"membership\": \"creator\","
        "  \"attributes\": {\"category\": \"str\", \"score\": \"dec\"}"
        "}";
    dispatch_req("POST", "/v1/collections", NULL, create_valid, &resp);
    ASSERT_EQ(201, resp.status);
    ASSERT(strstr(resp.body, "\"name\":\"demo.docs\"") != NULL);
    ASSERT(strstr(resp.body, "\"dimensions\":4") != NULL);
    ASSERT(strstr(resp.body, "\"stats\":{") != NULL);
    ASSERT(strstr(resp.body, "\"sync\":{") != NULL);
    free(resp.body);

    /* 4. POST /v1/collections duplicate -> 409 collection_exists */
    dispatch_req("POST", "/v1/collections", NULL, create_valid, &resp);
    ASSERT_EQ(409, resp.status);
    ASSERT(strstr(resp.body, "collection_exists") != NULL);
    free(resp.body);

    /* 5. GET /v1/collections/demo.docs -> 200 */
    dispatch_req("GET", "/v1/collections/demo.docs", NULL, NULL, &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "\"name\":\"demo.docs\"") != NULL);
    free(resp.body);

    /* 6. GET /v1/collections/nonexistent -> 404 */
    dispatch_req("GET", "/v1/collections/nonexistent", NULL, NULL, &resp);
    ASSERT_EQ(404, resp.status);
    ASSERT(strstr(resp.body, "collection_not_found") != NULL);
    free(resp.body);

    /* 7. GET /v1/collections -> has demo.docs */
    dispatch_req("GET", "/v1/collections", NULL, NULL, &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "\"name\":\"demo.docs\"") != NULL);
    free(resp.body);

    /* 8. DELETE /v1/collections/demo.docs -> 204 */
    dispatch_req("DELETE", "/v1/collections/demo.docs", NULL, NULL, &resp);
    ASSERT_EQ(204, resp.status);
    ASSERT(resp.body == NULL || resp.body_len == 0);

    /* 9. GET /v1/collections/demo.docs -> 404 */
    dispatch_req("GET", "/v1/collections/demo.docs", NULL, NULL, &resp);
    ASSERT_EQ(404, resp.status);
    free(resp.body);

    PASS();
}

TEST test_follow_collection(void)
{
    av_http_resp resp;

    /* Follow with definition */
    const char *follow_def =
        "{"
        "  \"definition\": {"
        "    \"name\": \"followed.docs\","
        "    \"column\": {\"type\": \"halfvec\", \"dimensions\": 4},"
        "    \"index\": {\"method\": \"hnsw\", \"opclass\": \"halfvec_cosine_ops\"},"
        "    \"writers\": [\"0x2222222222222222222222222222222222222222\"]"
        "  }"
        "}";
    dispatch_req("POST", "/v1/collections:follow", NULL, follow_def, &resp);
    ASSERT_EQ(201, resp.status);
    ASSERT(strstr(resp.body, "\"name\":\"followed.docs\"") != NULL);
    free(resp.body);

    /* Clean up */
    dispatch_req("DELETE", "/v1/collections/followed.docs", NULL, NULL, &resp);
    ASSERT_EQ(204, resp.status);

    PASS();
}

TEST test_entities_endpoint(void)
{
    av_http_resp resp;

    /* Setup collection */
    const char *create_col =
        "{"
        "  \"name\": \"ent.col\","
        "  \"column\": {\"type\": \"halfvec\", \"dimensions\": 4},"
        "  \"index\": {\"method\": \"none\", \"opclass\": \"halfvec_cosine_ops\"},"
        "  \"writers\": [\"0x1111111111111111111111111111111111111111\"]"
        "}";
    dispatch_req("POST", "/v1/collections", NULL, create_col, &resp);
    ASSERT_EQ(201, resp.status);
    free(resp.body);

    /* GET entity non-existent */
    dispatch_req("GET", "/v1/collections/ent.col/entities/doc-1", NULL, NULL, &resp);
    ASSERT_EQ(404, resp.status);
    ASSERT(strstr(resp.body, "entity_not_found") != NULL);
    free(resp.body);

    /* Verify=true -> 501 */
    dispatch_req("GET", "/v1/collections/ent.col/entities/doc-1", "verify=true", NULL, &resp);
    ASSERT_EQ(501, resp.status);
    ASSERT(strstr(resp.body, "Phase D6") != NULL);
    free(resp.body);

    /* 501 write endpoints */
    dispatch_req("POST", "/v1/collections/ent.col/entities", NULL, "{}", &resp);
    ASSERT_EQ(501, resp.status);
    free(resp.body);

    dispatch_req("DELETE", "/v1/collections/ent.col/entities/0x01", NULL, NULL, &resp);
    ASSERT_EQ(501, resp.status);
    free(resp.body);

    dispatch_req("POST", "/v1/collections/ent.col/entities:delete", NULL, "{}", &resp);
    ASSERT_EQ(501, resp.status);
    free(resp.body);

    dispatch_req("POST", "/v1/collections/ent.col/entities:extend", NULL, "{}", &resp);
    ASSERT_EQ(501, resp.status);
    free(resp.body);

    dispatch_req("POST", "/v1/collections/ent.col/entities:prepare", NULL, "{}", &resp);
    ASSERT_EQ(501, resp.status);
    free(resp.body);

    dispatch_req("DELETE", "/v1/collections/ent.col", NULL, NULL, &resp);
    ASSERT_EQ(204, resp.status);

    PASS();
}

TEST test_search_and_count(void)
{
    av_http_resp resp;

    /* Setup collection */
    const char *create_col =
        "{"
        "  \"name\": \"search.col\","
        "  \"column\": {\"type\": \"halfvec\", \"dimensions\": 4},"
        "  \"index\": {\"method\": \"none\", \"opclass\": \"halfvec_cosine_ops\"},"
        "  \"writers\": [\"0x1111111111111111111111111111111111111111\"]"
        "}";
    dispatch_req("POST", "/v1/collections", NULL, create_col, &resp);
    ASSERT_EQ(201, resp.status);
    free(resp.body);

    /* POST count */
    dispatch_req("POST", "/v1/collections/search.col/count", NULL, "{}", &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "\"count\":0") != NULL);
    free(resp.body);

    /* POST search valid (empty results) */
    const char *search_req =
        "{"
        "  \"vector\": [0.1, 0.2, 0.3, 0.4],"
        "  \"k\": 5,"
        "  \"explain\": true"
        "}";
    dispatch_req("POST", "/v1/collections/search.col/search", NULL, search_req, &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "\"results\":[]") != NULL);
    ASSERT(strstr(resp.body, "\"plan\":{") != NULL);
    ASSERT(strstr(resp.body, "\"strategy\":\"exact_flat\"") != NULL);
    free(resp.body);

    /* POST search invalid: neither vector nor vector_bytes */
    dispatch_req("POST", "/v1/collections/search.col/search", NULL, "{\"k\": 5}", &resp);
    ASSERT_EQ(422, resp.status);
    ASSERT(strstr(resp.body, "invalid_argument") != NULL);
    free(resp.body);

    /* POST search invalid: dimension mismatch */
    const char *search_dim_err = "{\"vector\": [0.1, 0.2]}";
    dispatch_req("POST", "/v1/collections/search.col/search", NULL, search_dim_err, &resp);
    ASSERT_EQ(422, resp.status);
    ASSERT(strstr(resp.body, "dimension_mismatch") != NULL);
    free(resp.body);

    /* POST search:batch -> 501 */
    dispatch_req("POST", "/v1/collections/search.col/search:batch", NULL, "{}", &resp);
    ASSERT_EQ(501, resp.status);
    ASSERT(strstr(resp.body, "Phase D4") != NULL);
    free(resp.body);

    /* POST scroll */
    dispatch_req("POST", "/v1/collections/search.col/scroll", NULL, "{\"limit\": 10}", &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "\"entities\":[]") != NULL);
    ASSERT(strstr(resp.body, "\"next_cursor\":null") != NULL);
    free(resp.body);

    /* Debug routes */
    dispatch_req("GET", "/_debug/collections/search.col/keys", NULL, NULL, &resp);
    ASSERT_EQ(200, resp.status);
    ASSERT(strstr(resp.body, "\"keys\":[]") != NULL);
    free(resp.body);

    dispatch_req("DELETE", "/v1/collections/search.col", NULL, NULL, &resp);
    ASSERT_EQ(204, resp.status);

    PASS();
}

SUITE(rest_suite)
{
    SET_SETUP(setup_rest, NULL);
    SET_TEARDOWN(teardown_rest, NULL);
    RUN_TEST(test_collections_crud);
    RUN_TEST(test_follow_collection);
    RUN_TEST(test_entities_endpoint);
    RUN_TEST(test_search_and_count);
}
