/*
 * test_ttl.c: TTL scheduler (heap order, extends, stale entries, removal, budget).
 */
#include <stdlib.h>

#include "sync/ttl.h"

#include "greatest.h"

TEST ttl_order_and_budget(void)
{
    av_ttl *t = av_ttl_create();
    for (uint32_t d = 0; d < 100; d++) {
        ASSERT_EQ(AV_OK, av_ttl_set(t, d, 1000 - d)); /* d=99 expires first */
    }
    uint32_t out[100];
    ASSERT_EQ(0u, av_ttl_pop_expired(t, 900, out, 100)); /* earliest is 901 */
    size_t n = av_ttl_pop_expired(t, 905, out, 3);       /* budget 3 of 5 due */
    ASSERT_EQ(3u, n);
    ASSERT_EQ(99u, out[0]);
    ASSERT_EQ(98u, out[1]);
    ASSERT_EQ(97u, out[2]);
    n = av_ttl_pop_expired(t, 905, out, 100);
    ASSERT_EQ(2u, n);
    ASSERT_EQ(96u, out[0]);
    ASSERT_EQ(95u, out[1]);
    av_ttl_destroy(t);
    PASS();
}

TEST ttl_extend_and_remove_make_entries_stale(void)
{
    av_ttl *t = av_ttl_create();
    av_ttl_set(t, 1, 10);
    av_ttl_set(t, 2, 10);
    av_ttl_set(t, 3, 10);
    av_ttl_set(t, 1, 20); /* extend */
    av_ttl_remove(t, 2);  /* deleted */
    av_ttl_set(t, 3, 5);  /* shortened: expires earlier */
    uint32_t out[8];
    size_t n = av_ttl_pop_expired(t, 10, out, 8);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(3u, out[0]);
    ASSERT_EQ(20u, av_ttl_expiry(t, 1));
    ASSERT_EQ(0u, av_ttl_expiry(t, 2));
    n = av_ttl_pop_expired(t, 19, out, 8);
    ASSERT_EQ(0u, n);
    n = av_ttl_pop_expired(t, 20, out, 8);
    ASSERT_EQ(1u, n);
    ASSERT_EQ(1u, out[0]);
    ASSERT_EQ(0u, av_ttl_heap_size(t)); /* all stale entries drained */
    av_ttl_destroy(t);
    PASS();
}

TEST ttl_compact_and_never(void)
{
    av_ttl *t = av_ttl_create();
    for (uint32_t i = 0; i < 1000; i++) {
        av_ttl_set(t, 7, 100 + i); /* 1000 extends of the same docid */
    }
    av_ttl_set(t, 8, 0); /* never expires */
    ASSERT_EQ(1000u, av_ttl_heap_size(t));
    av_ttl_compact(t);
    ASSERT_EQ(1u, av_ttl_heap_size(t));
    uint32_t out[4];
    ASSERT_EQ(1u, av_ttl_pop_expired(t, UINT64_MAX, out, 4));
    ASSERT_EQ(7u, out[0]);
    av_ttl_destroy(t);
    PASS();
}

TEST ttl_randomized_vs_model(void)
{
    enum { N = 500 };
    uint64_t model[N] = {0};
    av_ttl *t = av_ttl_create();
    unsigned seed = 3;
    uint64_t block = 0;
    for (int step = 0; step < 20000; step++) {
        uint32_t d = (uint32_t) (rand_r(&seed) % N);
        int op = rand_r(&seed) % 10;
        if (op < 6) {
            uint64_t exp = block + 1 + (uint64_t) (rand_r(&seed) % 50);
            av_ttl_set(t, d, exp);
            model[d] = exp;
        } else if (op < 7) {
            av_ttl_remove(t, d);
            model[d] = 0;
        } else {
            block++;
            uint32_t out[N];
            size_t n = av_ttl_pop_expired(t, block, out, N);
            size_t expect = 0;
            for (int i = 0; i < N; i++)
                if (model[i] != 0 && model[i] <= block)
                    expect++;
            ASSERT_EQ(expect, n);
            for (size_t i = 0; i < n; i++) {
                ASSERT(model[out[i]] != 0 && model[out[i]] <= block);
                model[out[i]] = 0;
            }
        }
    }
    av_ttl_destroy(t);
    PASS();
}

SUITE(ttl)
{
    RUN_TEST(ttl_order_and_budget);
    RUN_TEST(ttl_extend_and_remove_make_entries_stale);
    RUN_TEST(ttl_compact_and_never);
    RUN_TEST(ttl_randomized_vs_model);
}
