/*
 * test_rpc_parse.c: offline tests for the Arkiv client building blocks: entity JSON parser
 * (canonical encodings), log decoder, query builder and the RPC budget.
 */
#include <string.h>
#include <time.h>

#include "arkiv/budget.h"
#include "arkiv/entity.h"
#include "arkiv/logs.h"
#include "arkiv/query_builder.h"
#include "greatest.h"
#include "yyjson.h"

static yyjson_doc *J(const char *s)
{
    return yyjson_read(s, strlen(s), 0);
}

static int word_is(const av_attr *a, const char *hex64)
{
    uint8_t want[32];
    size_t n = 0;
    char buf[67] = "0x";
    strcat(buf, hex64);
    return a != NULL && a->len == 32 && av_hex_to_buf(buf, 66, want, 32, &n) && memcmp(want, a->val, 32) == 0;
}

#define Z48 "000000000000000000000000000000000000000000000000"
#define F48 "ffffffffffffffffffffffffffffffffffffffffffffffff"
#define F56 "ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
#define Z56 "00000000000000000000000000000000000000000000000000000000"

TEST entity_parse(void)
{
    static const char *src =
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\","
        "\"owner\":\"0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045\","
        "\"creator\":\"0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045\","
        "\"createdAt\":\"0x1bc\",\"updatedAt\":\"0x205\",\"expiresAt\":\"0x13d620\","
        "\"attributes\":["
        "{\"name\":\"arkiv.schema\",\"type\":\"str\",\"value\":\"my-app.posts\"},"
        "{\"name\":\"score\",\"type\":\"dec\",\"value\":\"4.5\"},"
        "{\"name\":\"neg\",\"type\":\"dec\",\"value\":\"-2.1\"},"
        "{\"name\":\"pub\",\"type\":\"bool\",\"value\":true},"
        "{\"name\":\"rep\",\"type\":\"i32\",\"value\":-42},"
        "{\"name\":\"ts\",\"type\":\"u64\",\"value\":\"0x65b3f210\"},"
        "{\"name\":\"bal\",\"type\":\"u256\",\"value\":\"0xde0b6b3a7640000\"},"
        "{\"name\":\"who\",\"type\":\"addr\",\"value\":\"0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045\"},"
        "{\"name\":\"av.vec\",\"type\":\"bytes\",\"value\":\"0x0102ff\"}]}";
    av_arena *ar = av_arena_create("t", 4096, 0);
    av_entity e;
    av_err err;
    yyjson_doc *d = J(src);
    ASSERT(d != NULL);
    ASSERT_EQ(AV_OK, av_entity_from_json(yyjson_doc_get_root(d), ar, &e, &err));
    ASSERT_EQ(0x9f, e.key[0]);
    ASSERT_EQ(0x01, e.key[31]);
    ASSERT_EQ(0xd8, e.owner[0]);
    ASSERT_EQ(0x1bc, e.created_at);
    ASSERT_EQ(0x205, e.updated_at);
    ASSERT_EQ(0x13d620, e.expires_at);
    ASSERT_EQ(9, e.n_attrs);
    const av_attr *a = av_entity_attr(&e, "arkiv.schema");
    ASSERT(a != NULL && a->type == AV_AT_STR && a->len == 12 && memcmp(a->val, "my-app.posts", 12) == 0);
    ASSERT(word_is(av_entity_attr(&e, "score"),
                   Z48 "3e73362871420000")); /* 4.5e18 (the doc example 0x3e7104b2a8d60000 is a typo) */
    ASSERT(word_is(av_entity_attr(&e, "neg"), F48 "e2db4d2053ae0000"));
    ASSERT(word_is(av_entity_attr(&e, "pub"), Z56 "00000001"));
    ASSERT(word_is(av_entity_attr(&e, "rep"), F56 "ffffffd6"));
    ASSERT(word_is(av_entity_attr(&e, "ts"), Z56 "65b3f210"));
    ASSERT(word_is(av_entity_attr(&e, "bal"), Z48 "0de0b6b3a7640000"));
    ASSERT(word_is(av_entity_attr(&e, "who"),
                   "000000000000000000000000d8da6bf26964af9d7eed9e03e53415d37aa96045"));
    a = av_entity_attr(&e, "av.vec");
    ASSERT(a != NULL && a->type == AV_AT_BYTES && a->len == 3 && a->val[2] == 0xff && !a->indexed);
    ASSERT(av_entity_attr(&e, "missing") == NULL);

    av_arena *a2 = av_arena_create("t2", 4096, 0);
    av_entity c;
    ASSERT_EQ(AV_OK, av_entity_copy(&e, a2, &c, &err));
    ASSERT(c.attrs != e.attrs && c.attrs[0].val != e.attrs[0].val);
    ASSERT(memcmp(c.attrs[8].val, "\x01\x02\xff", 3) == 0);
    yyjson_doc_free(d);
    av_arena_destroy(ar);
    av_arena_destroy(a2);
    PASS();
}

TEST entity_rejects(void)
{
    static const char *bad[] = {
        "[]",
        "{}",
        "{\"key\":\"0x12\"}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"createdAt\":"
        "\"12\"}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":{}}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":[{"
        "\"name\":\"a\",\"type\":\"zzz\",\"value\":1}]}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":[{"
        "\"name\":\"\",\"type\":\"bool\",\"value\":true}]}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":[{"
        "\"name\":\"a\",\"type\":\"i32\",\"value\":4294967296}]}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":[{"
        "\"name\":\"a\",\"type\":\"dec\",\"value\":\"1.0000000000000000001\"}]}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":[{"
        "\"name\":\"a\",\"type\":\"u64\",\"value\":\"0x10000000000000000\"}]}",
        "{\"key\":\"0x9f2c100000000000000000000000000000000000000000000000000000000001\",\"attributes\":[{"
        "\"name\":\"a\",\"type\":\"bytes\",\"value\":\"0xabc\"}]}",
    };
    av_arena *ar = av_arena_create("t", 4096, 0);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        av_entity e;
        av_err err;
        yyjson_doc *d = J(bad[i]);
        ASSERT(d != NULL);
        greatest_set_test_suffix(bad[i]);
        ASSERT_EQ(AV_ERR, av_entity_from_json(yyjson_doc_get_root(d), ar, &e, &err));
        ASSERT_STR_EQ("arkiv_rpc_error", err.code);
        yyjson_doc_free(d);
    }
    av_arena_destroy(ar);
    PASS();
}

static void topic_hex(av_event_kind k, char *out)
{
    av_key_to_hex(av_event_topic0(k), out);
}

TEST logs_decode(void)
{
    char t0[67];
    char json[2048];
    av_arena *ar = av_arena_create("t", 4096, 0);
    av_err err;
    av_event ev;
    bool known;
    static const char *key = "0x9f2c100000000000000000000000000000000000000000000000000000000001";
    static const char *own = "0x000000000000000000000000d8da6bf26964af9d7eed9e03e53415d37aa96045";
    static const char *common =
        "\"blockNumber\":\"0x10\",\"logIndex\":\"0x2\",\"removed\":true,"
        "\"blockHash\":\"0x1111111111111111111111111111111111111111111111111111111111111111\","
        "\"transactionHash\":\"0x2222222222222222222222222222222222222222222222222222222222222222\"";

    topic_hex(AV_EV_CREATED, t0);
    snprintf(json, sizeof json, "{\"topics\":[\"%s\",\"%s\",\"%s\"],\"data\":\"0x%s%s\",%s}", t0, key, own,
             Z56 "0000007b", Z56 "00000002", common);
    yyjson_doc *d = J(json);
    ASSERT_EQ(AV_OK, av_event_parse(yyjson_doc_get_root(d), &ev, &known, &err));
    ASSERT(known && ev.kind == AV_EV_CREATED && ev.expires_at == 0x7b && ev.flags == 2);
    ASSERT(ev.block == 16 && ev.log_index == 2 && ev.removed && ev.owner[0] == 0xd8 &&
           ev.block_hash[0] == 0x11);
    yyjson_doc_free(d);

    topic_hex(AV_EV_TRANSFERRED, t0);
    snprintf(json, sizeof json, "{\"topics\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"data\":\"0x\",%s}", t0, key, own,
             "0x0000000000000000000000002222222222222222222222222222222222222222", common);
    d = J(json);
    ASSERT_EQ(AV_OK, av_event_parse(yyjson_doc_get_root(d), &ev, &known, &err));
    ASSERT(known && ev.kind == AV_EV_TRANSFERRED && ev.prev_owner[0] == 0xd8 && ev.new_owner[0] == 0x22);
    yyjson_doc_free(d);

    /* wrong data length for a known event: error; unknown topic0: skipped */
    topic_hex(AV_EV_CREATED, t0);
    snprintf(json, sizeof json, "{\"topics\":[\"%s\",\"%s\",\"%s\"],\"data\":\"0x\",%s}", t0, key, own,
             common);
    d = J(json);
    ASSERT_EQ(AV_ERR, av_event_parse(yyjson_doc_get_root(d), &ev, &known, &err));
    yyjson_doc_free(d);
    d = J("{\"topics\":[\"0x1111111111111111111111111111111111111111111111111111111111111111\"],\"data\":"
          "\"0x\"}");
    ASSERT_EQ(AV_OK, av_event_parse(yyjson_doc_get_root(d), &ev, &known, &err));
    ASSERT(!known);
    yyjson_doc_free(d);

    av_event *evs;
    size_t n;
    d = J("[{\"topics\":[\"0x1111111111111111111111111111111111111111111111111111111111111111\"],\"data\":"
          "\"0x\"}]");
    ASSERT_EQ(AV_OK, av_events_parse(yyjson_doc_get_root(d), ar, &evs, &n, &err));
    ASSERT_EQ(0, n);
    yyjson_doc_free(d);
    av_arena_destroy(ar);
    PASS();
}

TEST query_builder_literals(void)
{
    char out[AV_QB_LIT_MAX];
    av_err err;
    ASSERT_EQ(AV_OK, av_qb_lit_str("Arkiv's", 7, out, sizeof out, &err));
    ASSERT_STR_EQ("str('Arkiv''s')", out);
    ASSERT_EQ(AV_OK, av_qb_lit_str("'); DROP", 8, out, sizeof out, &err));
    ASSERT_STR_EQ("str('''); DROP')", out);
    char big[200];
    memset(big, 'a', sizeof big);
    ASSERT_EQ(AV_OK, av_qb_lit_str(big, 128, out, sizeof out, &err));
    ASSERT_EQ(AV_ERR, av_qb_lit_str(big, 129, out, sizeof out, &err));
    ASSERT_EQ(AV_ERR, av_qb_lit_str("a\0b", 3, out, sizeof out, &err));
    ASSERT_EQ(AV_ERR, av_qb_lit_str("abc", 3, out, 8, &err)); /* needs 2*3+7 */
    uint8_t k[32] = {0};
    k[0] = 0xab;
    ASSERT_EQ(AV_OK, av_qb_lit_key(k, out, sizeof out, &err));
    ASSERT_EQ(0, strncmp(out, "key(0xab00", 10));
    ASSERT_EQ(AV_OK, av_qb_lit_u64(500000, out, sizeof out, &err));
    ASSERT_STR_EQ("u64(500000)", out);
    ASSERT_EQ(AV_OK, av_qb_lit_i32(-5, out, sizeof out, &err));
    ASSERT_STR_EQ("i32(-5)", out);
    ASSERT_EQ(AV_OK, av_qb_lit_dec("-4.5", out, sizeof out, &err));
    ASSERT_STR_EQ("dec(-4.5)", out);
    ASSERT_EQ(AV_ERR, av_qb_lit_dec("4.5)", out, sizeof out, &err));
    ASSERT_EQ(AV_ERR, av_qb_lit_u256("12a", out, sizeof out, &err));
    ASSERT_EQ(AV_OK, av_qb_lit_u256("1000000000000000000", out, sizeof out, &err));
    ASSERT(av_qb_valid_name("arkiv.schema") && av_qb_valid_name("a-b_c9"));
    ASSERT(!av_qb_valid_name("a b") && !av_qb_valid_name("1a") && !av_qb_valid_name("a--b") &&
           !av_qb_valid_name(""));
    PASS();
}

TEST query_builder_compose(void)
{
    char buf[1024];
    char lit[AV_QB_LIT_MAX];
    av_qb q;
    av_err err;
    av_qb_init(&q, buf, sizeof buf);
    ASSERT_EQ(AV_OK, av_qb_lit_str("my-app", 6, lit, sizeof lit, &err));
    ASSERT_EQ(AV_OK, av_qb_cmp(&q, "arkiv.schema", "=", lit, &err));
    ASSERT_EQ(AV_OK, av_qb_and(&q, &err));
    ASSERT_EQ(AV_OK, av_qb_lit_u64(5, lit, sizeof lit, &err));
    ASSERT_EQ(AV_OK, av_qb_cmp(&q, "$expiresAt", ">", lit, &err));
    ASSERT_STR_EQ("arkiv.schema = str('my-app') AND $expiresAt > u64(5)", buf);
    ASSERT_EQ(AV_ERR, av_qb_cmp(&q, "bad name", "=", lit, &err));
    ASSERT_EQ(AV_ERR, av_qb_cmp(&q, "a", "LIKE", lit, &err));

    uint8_t keys[3][32] = {{1}, {2}, {3}};
    ASSERT_EQ(AV_OK, av_qb_or_keys(buf, sizeof buf, keys, 3, &err));
    ASSERT(strstr(buf, " OR $key = key(0x02") != NULL && strncmp(buf, "$key = key(0x01", 15) == 0);
    ASSERT_EQ(AV_ERR, av_qb_or_keys(buf, 100, keys, 3, &err)); /* too long -> error, not truncation */
    uint8_t addrs[2][20] = {{1}, {2}};
    ASSERT_EQ(AV_OK, av_qb_or_creators(buf, sizeof buf, addrs, 2, &err));
    ASSERT(strncmp(buf, "$creator = addr(0x01", 20) == 0);

    ASSERT_EQ(AV_OK, av_qb_paged(buf, sizeof buf, NULL, NULL, &err));
    ASSERT(strstr(buf, "$key >= key(0x0000") != NULL && strstr(buf, "ORDER BY $key ASC") != NULL);
    ASSERT_EQ(AV_OK, av_qb_paged(buf, sizeof buf, "a = true", keys[1], &err));
    ASSERT(strncmp(buf, "(a = true) AND $key > key(0x02", 30) == 0);
    PASS();
}

TEST budget_blocks(void)
{
    av_budget *b = av_budget_create(20.0); /* 20 CU/s, burst 20 */
    ASSERT(b != NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 10; i++) /* 10 x 5 CU = 50 CU: 20 burst, 30 more at 20/s = 1.5 s */
        av_budget_acquire(b, av_budget_cost("arkiv_query"));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
    ASSERT(s > 1.2 && s < 3.0);
    ASSERT_EQ(50, av_budget_consumed(b));
    ASSERT_EQ(1, av_budget_cost("eth_blockNumber"));
    ASSERT_EQ(2, av_budget_cost("no_such_method"));
    av_budget_destroy(b);
    b = av_budget_create(0); /* unlimited */
    for (int i = 0; i < 1000; i++)
        av_budget_acquire(b, 5);
    ASSERT_EQ(0, av_budget_waited_ms(b));
    av_budget_destroy(b);
    PASS();
}

SUITE(arkiv_parse)
{
    RUN_TEST(entity_parse);
    RUN_TEST(entity_rejects);
    RUN_TEST(logs_decode);
    RUN_TEST(query_builder_literals);
    RUN_TEST(query_builder_compose);
    RUN_TEST(budget_blocks);
}
