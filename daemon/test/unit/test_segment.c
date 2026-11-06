/*
 * test_segment.c: unit tests for flat and HNSW segments (seal, footer, open_sealed).
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "greatest.h"
#include "index/segment.h"
#include "kernels/opclass.h"
#include "postgres.h"
#include "varatt.h"
#include "vector.h"

static Vector *alloc_test_vec(int dim, float base_val)
{
    Vector *v = malloc(VECTOR_SIZE((size_t) dim));
    if (v == NULL)
        return NULL;
    SET_VARSIZE(v, (uint32) VECTOR_SIZE((size_t) dim));
    v->dim = (int16) dim;
    v->unused = 0;
    for (int i = 0; i < dim; i++) {
        v->x[i] = base_val + (float) i * 0.05f;
    }
    return v;
}

TEST test_flat_segment_lifecycle(void)
{
    av_segment *seg = NULL;
    av_err err;
    av_err_clear(&err);

    av_status st = av_segment_create_flat(NULL, AV_VT_VECTOR, 4, 0, &seg, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(seg != NULL);
    ASSERT_EQ(AV_SEG_FLAT, av_segment_kind(seg));
    ASSERT_EQ(0u, (unsigned) av_segment_count(seg));

    Vector *v1 = alloc_test_vec(4, 1.0f);
    Vector *v2 = alloc_test_vec(4, 2.0f);
    ASSERT(v1 != NULL && v2 != NULL);

    st = av_segment_insert(seg, 1, v1, &err);
    ASSERT_EQ(AV_OK, st);
    st = av_segment_insert(seg, 2, v2, &err);
    ASSERT_EQ(AV_OK, st);

    ASSERT_EQ(2u, (unsigned) av_segment_count(seg));

    const void *got1 = av_segment_get(seg, 1);
    ASSERT(got1 != NULL);
    const void *got2 = av_segment_get(seg, 2);
    ASSERT(got2 != NULL);
    ASSERT(av_segment_get(seg, 99) == NULL);

    const av_opclass *oc = av_opclass_find("vector_l2_ops");
    ASSERT(oc != NULL);

    av_hit hits[4];
    size_t n_hits = 0;
    st = av_segment_scan_exact(seg, v1, oc, NULL, 2, hits, &n_hits, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(2u, (unsigned) n_hits);
    ASSERT_EQ(1u, hits[0].id);
    ASSERT(hits[0].distance < 1e-5);

    free(v1);
    free(v2);
    av_segment_destroy(seg);
    PASS();
}

TEST test_hnsw_segment_seal_and_open(void)
{
    char tmp_path[] = "/tmp/arkiv_test_hnsw_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT(fd >= 0);
    close(fd);
    unlink(tmp_path); /* let segment create it */

    av_segment *seg = NULL;
    av_err err;
    av_err_clear(&err);

    const av_opclass *oc = av_opclass_find("vector_l2_ops");
    ASSERT(oc != NULL);

    av_status st = av_segment_create_hnsw(tmp_path, AV_VT_VECTOR, 4, oc, 16, 64, 4 * 1024 * 1024, &seg, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(seg != NULL);
    ASSERT_EQ(AV_SEG_HNSW, av_segment_kind(seg));
    ASSERT(!av_segment_is_sealed(seg));

    for (uint32_t i = 1; i <= 20; i++) {
        Vector *v = alloc_test_vec(4, (float) i);
        st = av_segment_insert(seg, i, v, &err);
        free(v);
        ASSERT_EQ(AV_OK, st);
    }
    ASSERT_EQ(20u, (unsigned) av_segment_count(seg));

    /* Seal segment */
    st = av_segment_seal(seg, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(av_segment_is_sealed(seg));

    /* Insert on sealed segment must fail */
    Vector *v_extra = alloc_test_vec(4, 99.0f);
    st = av_segment_insert(seg, 21, v_extra, &err);
    free(v_extra);
    ASSERT_EQ(AV_ERR, st);

    /* Search sealed segment */
    Vector *q = alloc_test_vec(4, 5.0f);
    av_hit hits[5];
    size_t n_hits = 0;
    st = av_segment_search_hnsw(seg, q, 40, 1, 1000, NULL, 0, 5, hits, &n_hits, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(n_hits > 0);
    ASSERT_EQ(5u, hits[0].id);

    av_segment_destroy(seg);

    /* Open sealed from disk */
    av_segment *opened = NULL;
    st = av_segment_open_sealed(tmp_path, &opened, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT(opened != NULL);
    ASSERT(av_segment_is_sealed(opened));
    ASSERT_EQ(20u, (unsigned) av_segment_count(opened));

    /* Search on opened sealed segment gives identical result */
    av_hit hits2[5];
    size_t n_hits2 = 0;
    st = av_segment_search_hnsw(opened, q, 40, 1, 1000, NULL, 0, 5, hits2, &n_hits2, &err);
    ASSERT_EQ(AV_OK, st);
    ASSERT_EQ(n_hits, n_hits2);
    ASSERT_EQ(hits[0].id, hits2[0].id);

    free(q);
    av_segment_destroy(opened);
    unlink(tmp_path);
    PASS();
}

TEST test_hnsw_corrupt_footer_rejected(void)
{
    char tmp_path[] = "/tmp/arkiv_test_corrupt_XXXXXX";
    int fd = mkstemp(tmp_path);
    ASSERT(fd >= 0);
    close(fd);
    unlink(tmp_path);

    av_segment *seg = NULL;
    av_err err;
    av_err_clear(&err);

    const av_opclass *oc = av_opclass_find("vector_l2_ops");
    av_status st = av_segment_create_hnsw(tmp_path, AV_VT_VECTOR, 4, oc, 16, 64, 512 * 1024, &seg, &err);
    ASSERT_EQ(AV_OK, st);

    Vector *v = alloc_test_vec(4, 1.0f);
    st = av_segment_insert(seg, 1, v, &err);
    free(v);
    ASSERT_EQ(AV_OK, st);

    st = av_segment_seal(seg, &err);
    ASSERT_EQ(AV_OK, st);
    av_segment_destroy(seg);

    /* Corrupt the footer in file (flip bytes in footer magic) */
    FILE *f = fopen(tmp_path, "r+b");
    ASSERT(f != NULL);
    fseek(f, -(long) sizeof(av_seg_footer), SEEK_END);
    char junk[sizeof(av_seg_footer)];
    memset(junk, 0xFF, sizeof(junk));
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    av_segment *opened = NULL;
    st = av_segment_open_sealed(tmp_path, &opened, &err);
    ASSERT_EQ(AV_ERR, st);
    ASSERT(opened == NULL);

    unlink(tmp_path);
    PASS();
}

SUITE(segment_suite)
{
    RUN_TEST(test_flat_segment_lifecycle);
    RUN_TEST(test_hnsw_segment_seal_and_open);
    RUN_TEST(test_hnsw_corrupt_footer_rejected);
}
