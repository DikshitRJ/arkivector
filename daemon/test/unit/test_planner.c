/*
 * test_planner.c: tests for query planner (D3: exact flat).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"

#include "query/planner.h"

TEST test_planner_exact_flat(void)
{
    roaring_bitmap_t *cand = roaring_bitmap_create();
    for (uint32_t i = 1; i <= 25; i++) {
        roaring_bitmap_add(cand, i);
    }

    av_query_plan plan;
    av_err err;
    av_err_clear(&err);

    av_status st = av_plan_query(NULL, NULL, NULL, false, cand, 100, &plan, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(AV_STRAT_EXACT_FLAT, plan.strategy);
    ASSERT(plan.exact);
    ASSERT_EQ(25u, (unsigned) plan.candidates);
    ASSERT(fabs(plan.selectivity - 0.25) < 1e-6);
    ASSERT(plan.kernel != NULL);
    ASSERT_STR_EQ("exact_flat", av_plan_strategy_name(plan.strategy));

    roaring_bitmap_free(cand);
    PASS();
}

TEST test_planner_null_candidates(void)
{
    av_query_plan plan;
    av_err err;
    av_err_clear(&err);

    av_status st = av_plan_query(NULL, NULL, NULL, false, NULL, 150, &plan, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(AV_STRAT_EXACT_FLAT, plan.strategy);
    ASSERT(plan.exact);
    ASSERT_EQ(150u, (unsigned) plan.candidates);
    ASSERT(fabs(plan.selectivity - 1.0) < 1e-6);

    PASS();
}

TEST test_planner_empty_candidates(void)
{
    roaring_bitmap_t *cand = roaring_bitmap_create();
    av_query_plan plan;
    av_err err;
    av_err_clear(&err);

    av_status st = av_plan_query(NULL, NULL, NULL, false, cand, 50, &plan, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(AV_STRAT_EXACT_FLAT, plan.strategy);
    ASSERT(plan.exact);
    ASSERT_EQ(0u, (unsigned) plan.candidates);
    ASSERT(fabs(plan.selectivity - 0.0) < 1e-6);

    roaring_bitmap_free(cand);
    PASS();
}

TEST test_planner_zero_live(void)
{
    av_query_plan plan;
    av_err err;
    av_err_clear(&err);

    av_status st = av_plan_query(NULL, NULL, NULL, false, NULL, 0, &plan, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(AV_STRAT_EXACT_FLAT, plan.strategy);
    ASSERT(plan.exact);
    ASSERT_EQ(0u, (unsigned) plan.candidates);
    ASSERT(fabs(plan.selectivity - 1.0) < 1e-6);

    PASS();
}

TEST test_planner_force_exact(void)
{
    roaring_bitmap_t *cand = roaring_bitmap_create();
    roaring_bitmap_add(cand, 1);
    roaring_bitmap_add(cand, 2);

    av_query_plan plan;
    av_err err;
    av_err_clear(&err);

    av_status st = av_plan_query(NULL, NULL, NULL, true, cand, 10, &plan, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(AV_STRAT_EXACT_FLAT, plan.strategy);
    ASSERT(plan.exact);
    ASSERT_EQ(2u, (unsigned) plan.candidates);
    ASSERT(fabs(plan.selectivity - 0.2) < 1e-6);

    roaring_bitmap_free(cand);
    PASS();
}

TEST test_planner_strategy_names(void)
{
    ASSERT_STR_EQ("exact_flat", av_plan_strategy_name(AV_STRAT_EXACT_FLAT));
    ASSERT_STR_EQ("hnsw", av_plan_strategy_name(AV_STRAT_HNSW));
    ASSERT_STR_EQ("hnsw_iterative", av_plan_strategy_name(AV_STRAT_HNSW_ITERATIVE));
    ASSERT_STR_EQ("mixed", av_plan_strategy_name(AV_STRAT_MIXED));
    ASSERT_STR_EQ("remote_prefilter", av_plan_strategy_name(AV_STRAT_REMOTE_PREFILTER));
    ASSERT_STR_EQ("remote_postfilter", av_plan_strategy_name(AV_STRAT_REMOTE_POSTFILTER));
    PASS();
}

SUITE(planner)
{
    RUN_TEST(test_planner_exact_flat);
    RUN_TEST(test_planner_null_candidates);
    RUN_TEST(test_planner_empty_candidates);
    RUN_TEST(test_planner_zero_live);
    RUN_TEST(test_planner_force_exact);
    RUN_TEST(test_planner_strategy_names);
}
