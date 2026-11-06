/*
 * test_filter_eval.c: tests evaluating filter ASTs against av_attr_index and av_docmap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"

#include "core/arena.h"
#include "filter/ast.h"
#include "filter/eval.h"
#include "filter/parser.h"
#include "filter/value.h"
#include "index/attr_index.h"
#include "index/docid.h"

typedef struct {
    av_attr_index *ix;
    av_docmap *dm;
    roaring_bitmap_t *live;
    av_arena *arena;
} test_fixture;

static void make_key(uint8_t byte_val, uint8_t key[32])
{
    memset(key, 0, 32);
    key[0] = byte_val;
}

static void add_i32(av_attr_index *ix, av_docid id, const char *name, int32_t val)
{
    av_value v;
    av_value_i32(&v, val);
    av_err err;
    av_err_clear(&err);
    av_attr_index_insert(ix, id, name, AV_AT_I32, v.b, v.len, &err);
}

static void add_dec(av_attr_index *ix, av_docid id, const char *name, const char *dec_str)
{
    av_value v;
    av_value_parse(AV_AT_DEC, dec_str, strlen(dec_str), &v, NULL);
    av_err err;
    av_err_clear(&err);
    av_attr_index_insert(ix, id, name, AV_AT_DEC, v.b, v.len, &err);
}

static void add_str(av_attr_index *ix, av_docid id, const char *name, const char *s)
{
    av_value v;
    av_value_str(&v, (const uint8_t *) s, strlen(s));
    av_err err;
    av_err_clear(&err);
    av_attr_index_insert(ix, id, name, AV_AT_STR, v.b, v.len, &err);
}

static void setup_fixture(test_fixture *fix)
{
    fix->ix = av_attr_index_create();
    fix->dm = av_docmap_create();
    fix->live = roaring_bitmap_create();
    fix->arena = av_arena_create("filter_eval", 65536, 0);

    av_err err;
    av_err_clear(&err);

    /* Doc 1: key=0x01..., exp=1000, rank=10, score=1.5, tag="alpha" */
    uint8_t k1[32];
    make_key(1, k1);
    av_docmap_assign(fix->dm, k1, 1, 100, 1000, &err);
    roaring_bitmap_add(fix->live, 1);
    add_i32(fix->ix, 1, "rank", 10);
    add_dec(fix->ix, 1, "score", "1.5");
    add_str(fix->ix, 1, "tag", "alpha");

    /* Doc 2: key=0x02..., exp=1000, rank=20, score=2.5, tag="alpine" */
    uint8_t k2[32];
    make_key(2, k2);
    av_docmap_assign(fix->dm, k2, 2, 100, 1000, &err);
    roaring_bitmap_add(fix->live, 2);
    add_i32(fix->ix, 2, "rank", 20);
    add_dec(fix->ix, 2, "score", "2.5");
    add_str(fix->ix, 2, "tag", "alpine");

    /* Doc 3: key=0x03..., exp=1000, rank=30, score=3.5, tag="beta" */
    uint8_t k3[32];
    make_key(3, k3);
    av_docmap_assign(fix->dm, k3, 3, 100, 1000, &err);
    roaring_bitmap_add(fix->live, 3);
    add_i32(fix->ix, 3, "rank", 30);
    add_dec(fix->ix, 3, "score", "3.5");
    add_str(fix->ix, 3, "tag", "beta");

    /* Doc 4: key=0x04..., exp=50 (expires early!), rank=40, score=4.5, tag="gamma" */
    uint8_t k4[32];
    make_key(4, k4);
    av_docmap_assign(fix->dm, k4, 4, 100, 50, &err);
    roaring_bitmap_add(fix->live, 4);
    add_i32(fix->ix, 4, "rank", 40);
    add_dec(fix->ix, 4, "score", "4.5");
    add_str(fix->ix, 4, "tag", "gamma");

    /* Doc 5: key=0x05..., exp=1000, rank=dec(50.0), note="solo" */
    uint8_t k5[32];
    make_key(5, k5);
    av_docmap_assign(fix->dm, k5, 5, 100, 1000, &err);
    roaring_bitmap_add(fix->live, 5);
    add_dec(fix->ix, 5, "rank", "50.0");
    add_str(fix->ix, 5, "note", "solo");

    /* Doc 6: not in live bitmap */
    uint8_t k6[32];
    make_key(6, k6);
    av_docmap_assign(fix->dm, k6, 6, 100, 1000, &err);
    add_i32(fix->ix, 6, "rank", 60);

    av_attr_index_flush(fix->ix, &err);
}

static void teardown_fixture(test_fixture *fix)
{
    av_attr_index_destroy(fix->ix);
    av_docmap_destroy(fix->dm);
    roaring_bitmap_free(fix->live);
    av_arena_destroy(fix->arena);
}

static roaring_bitmap_t *eval_where(test_fixture *fix, const char *where, uint64_t as_of_block)
{
    av_filter *f = NULL;
    av_err err;
    av_err_clear(&err);
    av_status st = av_filter_from_where(where, strlen(where), NULL, fix->arena, &f, &err);
    if (st != AV_OK) {
        return NULL;
    }
    return av_filter_eval(f, fix->ix, fix->dm, fix->live, as_of_block, &err);
}

static bool bitmap_matches(const roaring_bitmap_t *bm, const uint32_t *expected, size_t count)
{
    if (bm == NULL)
        return false;
    if (roaring_bitmap_get_cardinality(bm) != count)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (!roaring_bitmap_contains(bm, expected[i]))
            return false;
    }
    return true;
}

TEST test_true_filter(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* NULL filter */
    av_err err;
    av_err_clear(&err);
    roaring_bitmap_t *bm = av_filter_eval(NULL, fix.ix, fix.dm, fix.live, 0, &err);
    ASSERT(bm != NULL);
    uint32_t exp5[] = {1, 2, 3, 4, 5};
    ASSERT(bitmap_matches(bm, exp5, 5));
    roaring_bitmap_free(bm);

    /* Explicit TRUE filter */
    av_filter *ftrue = av_filter_true(fix.arena);
    ASSERT(ftrue != NULL);
    bm = av_filter_eval(ftrue, fix.ix, fix.dm, fix.live, 0, &err);
    ASSERT(bm != NULL);
    ASSERT(bitmap_matches(bm, exp5, 5));
    roaring_bitmap_free(bm);

    /* Expired pruning with TRUE filter */
    bm = av_filter_eval(ftrue, fix.ix, fix.dm, fix.live, 50, &err);
    ASSERT(bm != NULL);
    uint32_t exp4[] = {1, 2, 3, 5};
    ASSERT(bitmap_matches(bm, exp4, 4));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_cmp_i32(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* EQ */
    roaring_bitmap_t *bm = eval_where(&fix, "rank = 20", 0);
    uint32_t e2[] = {2};
    ASSERT(bitmap_matches(bm, e2, 1));
    roaring_bitmap_free(bm);

    /* NE (does not match doc 5 since doc 5 has rank as dec, not i32) */
    bm = eval_where(&fix, "rank != 20", 0);
    uint32_t ene[] = {1, 3, 4};
    ASSERT(bitmap_matches(bm, ene, 3));
    roaring_bitmap_free(bm);

    /* LT */
    bm = eval_where(&fix, "rank < 30", 0);
    uint32_t elt[] = {1, 2};
    ASSERT(bitmap_matches(bm, elt, 2));
    roaring_bitmap_free(bm);

    /* LE */
    bm = eval_where(&fix, "rank <= 20", 0);
    uint32_t ele[] = {1, 2};
    ASSERT(bitmap_matches(bm, ele, 2));
    roaring_bitmap_free(bm);

    /* GT */
    bm = eval_where(&fix, "rank > 20", 0);
    uint32_t egt[] = {3, 4};
    ASSERT(bitmap_matches(bm, egt, 2));
    roaring_bitmap_free(bm);

    /* GE */
    bm = eval_where(&fix, "rank >= 30", 0);
    uint32_t ege[] = {3, 4};
    ASSERT(bitmap_matches(bm, ege, 2));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_cmp_dec(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* EQ */
    roaring_bitmap_t *bm = eval_where(&fix, "score = dec(2.5)", 0);
    uint32_t e2[] = {2};
    ASSERT(bitmap_matches(bm, e2, 1));
    roaring_bitmap_free(bm);

    /* NE */
    bm = eval_where(&fix, "score != dec(2.5)", 0);
    uint32_t ene[] = {1, 3, 4};
    ASSERT(bitmap_matches(bm, ene, 3));
    roaring_bitmap_free(bm);

    /* LT */
    bm = eval_where(&fix, "score < dec(3.0)", 0);
    uint32_t elt[] = {1, 2};
    ASSERT(bitmap_matches(bm, elt, 2));
    roaring_bitmap_free(bm);

    /* LE */
    bm = eval_where(&fix, "score <= dec(2.5)", 0);
    uint32_t ele[] = {1, 2};
    ASSERT(bitmap_matches(bm, ele, 2));
    roaring_bitmap_free(bm);

    /* GT */
    bm = eval_where(&fix, "score > dec(2.5)", 0);
    uint32_t egt[] = {3, 4};
    ASSERT(bitmap_matches(bm, egt, 2));
    roaring_bitmap_free(bm);

    /* GE */
    bm = eval_where(&fix, "score >= dec(3.5)", 0);
    uint32_t ege[] = {3, 4};
    ASSERT(bitmap_matches(bm, ege, 2));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_between_numbers(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* BETWEEN on i32 */
    roaring_bitmap_t *bm = eval_where(&fix, "rank BETWEEN 15 AND 35", 0);
    uint32_t e23[] = {2, 3};
    ASSERT(bitmap_matches(bm, e23, 2));
    roaring_bitmap_free(bm);

    /* BETWEEN on dec */
    bm = eval_where(&fix, "score BETWEEN dec(2.0) AND dec(4.0)", 0);
    ASSERT(bitmap_matches(bm, e23, 2));
    roaring_bitmap_free(bm);

    /* BETWEEN matching single */
    bm = eval_where(&fix, "rank BETWEEN 20 AND 20", 0);
    uint32_t e2[] = {2};
    ASSERT(bitmap_matches(bm, e2, 1));
    roaring_bitmap_free(bm);

    /* BETWEEN inverted range (matches nothing) */
    bm = eval_where(&fix, "rank BETWEEN 35 AND 15", 0);
    ASSERT_EQ(0u, roaring_bitmap_get_cardinality(bm));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_between_key(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* Key range covering docs 2, 3, 4 */
    const char *q = "$key BETWEEN key(0x0200000000000000000000000000000000000000000000000000000000000000) "
                    "AND key(0x0400000000000000000000000000000000000000000000000000000000000000)";
    roaring_bitmap_t *bm = eval_where(&fix, q, 0);
    uint32_t exp234[] = {2, 3, 4};
    ASSERT(bitmap_matches(bm, exp234, 3));
    roaring_bitmap_free(bm);

    /* Key range covering single doc */
    const char *qsingle = "$key BETWEEN key(0x0300000000000000000000000000000000000000000000000000000000000000) "
                          "AND key(0x0300000000000000000000000000000000000000000000000000000000000000)";
    bm = eval_where(&fix, qsingle, 0);
    uint32_t exp3[] = {3};
    ASSERT(bitmap_matches(bm, exp3, 1));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_startswith_str(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* Empty prefix: matches all entities with tag as str (docs 1, 2, 3, 4) */
    roaring_bitmap_t *bm = eval_where(&fix, "tag STARTSWITH str('')", 0);
    uint32_t exp1234[] = {1, 2, 3, 4};
    ASSERT(bitmap_matches(bm, exp1234, 4));
    roaring_bitmap_free(bm);

    /* Match prefix "alp": matches 1 ("alpha") and 2 ("alpine") */
    bm = eval_where(&fix, "tag STARTSWITH str('alp')", 0);
    uint32_t exp12[] = {1, 2};
    ASSERT(bitmap_matches(bm, exp12, 2));
    roaring_bitmap_free(bm);

    /* Mismatch prefix: matches none */
    bm = eval_where(&fix, "tag STARTSWITH str('xyz')", 0);
    ASSERT_EQ(0u, roaring_bitmap_get_cardinality(bm));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_exists(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* User attribute exists (all 5 docs have rank, either i32 or dec) */
    roaring_bitmap_t *bm = eval_where(&fix, "EXISTS(rank)", 0);
    uint32_t exp12345[] = {1, 2, 3, 4, 5};
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* User attribute note (only doc 5) */
    bm = eval_where(&fix, "EXISTS(note)", 0);
    uint32_t exp5[] = {5};
    ASSERT(bitmap_matches(bm, exp5, 1));
    roaring_bitmap_free(bm);

    /* Missing user attribute */
    bm = eval_where(&fix, "EXISTS(nonexistent)", 0);
    ASSERT_EQ(0u, roaring_bitmap_get_cardinality(bm));
    roaring_bitmap_free(bm);

    /* System field $key */
    bm = eval_where(&fix, "EXISTS($key)", 0);
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* System field $owner */
    bm = eval_where(&fix, "EXISTS($owner)", 0);
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* System field $creator */
    bm = eval_where(&fix, "EXISTS($creator)", 0);
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* System field $expiresAt */
    bm = eval_where(&fix, "EXISTS($expiresAt)", 0);
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_typeof(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* User attribute rank as i32 (docs 1..4) */
    roaring_bitmap_t *bm = eval_where(&fix, "TYPEOF(rank) = i32", 0);
    uint32_t exp1234[] = {1, 2, 3, 4};
    ASSERT(bitmap_matches(bm, exp1234, 4));
    roaring_bitmap_free(bm);

    /* User attribute rank as dec (doc 5) */
    bm = eval_where(&fix, "TYPEOF(rank) = dec", 0);
    uint32_t exp5[] = {5};
    ASSERT(bitmap_matches(bm, exp5, 1));
    roaring_bitmap_free(bm);

    /* User attribute tag as str (docs 1..4) */
    bm = eval_where(&fix, "TYPEOF(tag) = str", 0);
    ASSERT(bitmap_matches(bm, exp1234, 4));
    roaring_bitmap_free(bm);

    /* System field $key */
    bm = eval_where(&fix, "TYPEOF($key) = key", 0);
    uint32_t exp12345[] = {1, 2, 3, 4, 5};
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* System field $owner */
    bm = eval_where(&fix, "TYPEOF($owner) = addr", 0);
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* System field $expiresAt */
    bm = eval_where(&fix, "TYPEOF($expiresAt) = u64", 0);
    ASSERT(bitmap_matches(bm, exp12345, 5));
    roaring_bitmap_free(bm);

    /* Mismatched type on system field */
    bm = eval_where(&fix, "TYPEOF($key) = i32", 0);
    ASSERT_EQ(0u, roaring_bitmap_get_cardinality(bm));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_and_or_not_combinations(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* AND */
    roaring_bitmap_t *bm = eval_where(&fix, "rank >= 20 AND score <= dec(3.5)", 0);
    uint32_t exp23[] = {2, 3};
    ASSERT(bitmap_matches(bm, exp23, 2));
    roaring_bitmap_free(bm);

    /* OR */
    bm = eval_where(&fix, "rank = 10 OR rank = 30", 0);
    uint32_t exp13[] = {1, 3};
    ASSERT(bitmap_matches(bm, exp13, 2));
    roaring_bitmap_free(bm);

    /* NOT (complements over live set: excludes 2, matches 1, 3, 4, 5) */
    bm = eval_where(&fix, "NOT (rank = 20)", 0);
    uint32_t exp_not2[] = {1, 3, 4, 5};
    ASSERT(bitmap_matches(bm, exp_not2, 4));
    roaring_bitmap_free(bm);

    /* Complex combination: (rank = 10 OR rank = 20) AND NOT (score > dec(2.0)) */
    bm = eval_where(&fix, "(rank = 10 OR rank = 20) AND NOT (score > dec(2.0))", 0);
    uint32_t exp1[] = {1};
    ASSERT(bitmap_matches(bm, exp1, 1));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

TEST test_expiration_pruning(void)
{
    test_fixture fix;
    setup_fixture(&fix);

    /* Doc 4 expires at block 50. With as_of_block = 0, doc 4 is live */
    roaring_bitmap_t *bm = eval_where(&fix, "rank >= 30", 0);
    uint32_t exp34[] = {3, 4};
    ASSERT(bitmap_matches(bm, exp34, 2));
    roaring_bitmap_free(bm);

    /* With as_of_block = 50, doc 4 is pruned (expires_at <= 50) */
    bm = eval_where(&fix, "rank >= 30", 50);
    uint32_t exp3[] = {3};
    ASSERT(bitmap_matches(bm, exp3, 1));
    roaring_bitmap_free(bm);

    /* With as_of_block = 100, doc 4 is still pruned */
    bm = eval_where(&fix, "rank = 40", 100);
    ASSERT_EQ(0u, roaring_bitmap_get_cardinality(bm));
    roaring_bitmap_free(bm);

    /* With as_of_block = 1000, all entities expired */
    bm = eval_where(&fix, "EXISTS(rank)", 1000);
    ASSERT_EQ(0u, roaring_bitmap_get_cardinality(bm));
    roaring_bitmap_free(bm);

    teardown_fixture(&fix);
    PASS();
}

SUITE(filter_eval)
{
    RUN_TEST(test_true_filter);
    RUN_TEST(test_cmp_i32);
    RUN_TEST(test_cmp_dec);
    RUN_TEST(test_between_numbers);
    RUN_TEST(test_between_key);
    RUN_TEST(test_startswith_str);
    RUN_TEST(test_exists);
    RUN_TEST(test_typeof);
    RUN_TEST(test_and_or_not_combinations);
    RUN_TEST(test_expiration_pruning);
}
