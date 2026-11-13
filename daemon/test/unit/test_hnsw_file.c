/*
 * test_hnsw_file.c: HNSW band files (issue #11): snapshot -> encode -> decode round trip, searches
 * equal on the restored graph, inserts keep working after a restore, every header check, a sweep of
 * single-byte corruptions (rejected or harmless, never a crash), the flat-segment verification, and
 * av_ttl_count_due. Setting AV_WRITE_HNSW_SEED=<path> writes a fuzz seed (selector byte + file).
 */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "greatest.h"
#include "index/hnsw_index.h"
#include "index/segment.h"
#include "kernels/opclass.h"
#include "postgres.h"
#include "sync/ttl.h"
#include "varatt.h"
#include "vector.h"

#define DIM 3
#define N   60

static Vector *vec3(float a, float b, float c)
{
    Vector *v = malloc(VECTOR_SIZE((size_t) DIM));
    SET_VARSIZE(v, (uint32) VECTOR_SIZE((size_t) DIM));
    v->dim = DIM;
    v->unused = 0;
    v->x[0] = a;
    v->x[1] = b;
    v->x[2] = c;
    return v;
}

static Vector *vec_i(int i)
{
    return vec3(sinf((float) i), cosf((float) i * 0.7f), (float) (i % 7) * 0.1f);
}

/* A graph over docids 0..n-1 plus the matching flat segment. */
static void build(int n, av_segment **g, av_segment **flat)
{
    av_err e;
    av_err_clear(&e);
    const av_opclass *oc = av_opclass_find("vector_l2_ops");
    size_t reserve = av_segment_hnsw_reserve(AV_VT_VECTOR, DIM, 4, 1000);
    (void) av_segment_create_hnsw(NULL, AV_VT_VECTOR, DIM, oc, 4, 16, reserve, g, &e);
    (void) av_segment_create_flat(NULL, AV_VT_VECTOR, DIM, (size_t) 1 << 24, flat, &e);
    for (int i = 0; i < n; i++) {
        Vector *v = vec_i(i);
        (void) av_segment_insert(*g, (av_docid) i, v, &e);
        (void) av_segment_insert(*flat, (av_docid) i, v, &e);
        free(v);
    }
}

static av_hnsw_file_expect expect_default(void)
{
    av_hnsw_file_expect ex = {AV_VT_VECTOR, DIM, 4, 16, "vector_l2_ops", 7, 100, 1000};
    return ex;
}

static uint8_t *encode(const av_segment *g, size_t *len)
{
    void *img = NULL;
    size_t il = 0;
    av_seg_hnsw_info info;
    av_err e;
    av_err_clear(&e);
    if (av_segment_hnsw_snapshot(g, &img, &il, &info, &e) != AV_OK)
        return NULL;
    uint8_t *out = NULL;
    if (av_hnsw_file_encode(img, il, &info, "vector_l2_ops", 7, 100, 1000, 42, &out, len, &e) != AV_OK)
        out = NULL;
    free(img);
    return out;
}

static av_status decode(const uint8_t *buf, size_t len, const av_hnsw_file_expect *ex, bool crc,
                        av_segment **out)
{
    av_err e;
    av_err_clear(&e);
    av_hnsw_file_info fi;
    return av_hnsw_file_decode(buf, len, ex, av_opclass_find("vector_l2_ops"), crc, (size_t) 1 << 24, out,
                               &fi, &e);
}

TEST hnsw_file_round_trip(void)
{
    av_segment *g = NULL, *flat = NULL;
    build(N, &g, &flat);
    size_t len = 0;
    uint8_t *buf = encode(g, &len);
    ASSERT(buf != NULL);
    const char *seed = getenv("AV_WRITE_HNSW_SEED");
    if (seed != NULL) {
        FILE *f = fopen(seed, "wb");
        if (f != NULL) {
            fputc(0, f); /* fuzz selector: check_crc off */
            fwrite(buf, 1, len, f);
            fclose(f);
        }
    }
    av_hnsw_file_expect ex = expect_default();
    av_segment *r = NULL;
    ASSERT_EQ(AV_OK, decode(buf, len, &ex, true, &r));
    ASSERT_EQ(av_segment_count(g), av_segment_count(r));

    /* identical searches on the original and the restored graph */
    for (int q = 0; q < 10; q++) {
        Vector *qv = vec3((float) q * 0.3f - 1.0f, 0.5f, 0.2f);
        av_hit a[5], b[5];
        size_t na = 0, nb = 0;
        av_err e;
        ASSERT_EQ(AV_OK, av_segment_search_hnsw(g, qv, 40, 0, 0, NULL, 0, 5, a, &na, &e));
        ASSERT_EQ(AV_OK, av_segment_search_hnsw(r, qv, 40, 0, 0, NULL, 0, 5, b, &nb, &e));
        ASSERT_EQ(na, nb);
        for (size_t i = 0; i < na; i++)
            ASSERT_EQ(a[i].id, b[i].id);
        free(qv);
    }
    /* the restored graph takes inserts */
    av_err e;
    Vector *nv = vec3(9.0f, 9.0f, 9.0f);
    ASSERT_EQ(AV_OK, av_segment_insert(r, N + 5, nv, &e));
    av_hit h[1];
    size_t nh = 0;
    ASSERT_EQ(AV_OK, av_segment_search_hnsw(r, nv, 40, 0, 0, NULL, 0, 1, h, &nh, &e));
    ASSERT_EQ(1u, (unsigned) nh);
    ASSERT_EQ(N + 5, (int) h[0].id);
    free(nv);

    /* verification against the flat segment: equal, then a reassigned docid is detected */
    roaring_bitmap_t *live = roaring_bitmap_from_range(0, N, 1);
    size_t bad = 99;
    ASSERT_EQ(AV_OK, av_segment_hnsw_verify(r, flat, live, &bad, &e));
    ASSERT_EQ(0u, (unsigned) bad);
    av_segment *flat2 = NULL;
    (void) av_segment_create_flat(NULL, AV_VT_VECTOR, DIM, (size_t) 1 << 24, &flat2, &e);
    for (int i = 0; i < N; i++) {
        Vector *v = vec_i(i == 3 ? 1000 : i);
        (void) av_segment_insert(flat2, (av_docid) i, v, &e);
        free(v);
    }
    ASSERT_EQ(AV_OK, av_segment_hnsw_verify(r, flat2, live, &bad, &e));
    ASSERT_EQ(1u, (unsigned) bad);

    roaring_bitmap_free(live);
    av_segment_destroy(flat2);
    av_segment_destroy(r);
    av_segment_destroy(g);
    av_segment_destroy(flat);
    free(buf);
    PASS();
}

TEST hnsw_file_header_checks(void)
{
    av_segment *g = NULL, *flat = NULL;
    build(N, &g, &flat);
    size_t len = 0;
    uint8_t *buf = encode(g, &len);
    ASSERT(buf != NULL);
    av_segment *r = NULL;
    av_hnsw_file_expect ex = expect_default();

    ex.m = 8;
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    ex = expect_default();
    ex.opclass = "vector_cosine_ops";
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    ex = expect_default();
    ex.band = 8;
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    ex = expect_default();
    ex.band_blocks = 0;
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    ex = expect_default();
    ex.next_docid = N - 1; /* the store has not committed every docid of the graph */
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    ex = expect_default();
    ASSERT_EQ(AV_ERR, decode(buf, len - 8, &ex, true, &r));
    ASSERT_EQ(AV_ERR, decode(buf, 100, &ex, true, &r));
    buf[len - 1] ^= 1; /* payload checksum */
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    buf[len - 1] ^= 1;
    buf[20] ^= 1; /* header checksum */
    ASSERT_EQ(AV_ERR, decode(buf, len, &ex, true, &r));
    buf[20] ^= 1;
    ASSERT_EQ(AV_OK, decode(buf, len, &ex, true, &r));
    av_segment_destroy(r);
    av_segment_destroy(g);
    av_segment_destroy(flat);
    free(buf);
    PASS();
}

/* Every single-byte corruption of the image, with the CRC check off so the structural validator
 * sees them: either rejected, or (padding / distances / unused fields) a graph that still searches. */
TEST hnsw_file_corruption_sweep(void)
{
    av_segment *g = NULL, *flat = NULL;
    build(20, &g, &flat);
    size_t len = 0;
    uint8_t *buf = encode(g, &len);
    ASSERT(buf != NULL);
    av_hnsw_file_expect ex = expect_default();
    uint8_t *copy = malloc(len);
    Vector *qv = vec3(0.1f, 0.2f, 0.3f);
    size_t accepted = 0, rejected = 0;
    for (size_t off = AV_HNSW_FILE_HEADER; off < len; off++) {
        for (int bit = 0; bit < 8; bit += 3) {
            memcpy(copy, buf, len);
            copy[off] ^= (uint8_t) (1u << bit);
            av_segment *r = NULL;
            if (decode(copy, len, &ex, false, &r) == AV_OK) {
                av_hit h[5];
                size_t nh = 0;
                av_err e;
                (void) av_segment_search_hnsw(r, qv, 40, 1, 200, NULL, 0, 5, h, &nh, &e);
                av_segment_destroy(r);
                accepted++;
            } else {
                rejected++;
            }
        }
    }
    ASSERT(rejected > 0);
    ASSERT(accepted + rejected > 0);
    free(qv);
    free(copy);
    free(buf);
    av_segment_destroy(g);
    av_segment_destroy(flat);
    PASS();
}

TEST ttl_count_due(void)
{
    av_ttl *t = av_ttl_create();
    for (uint32_t i = 0; i < 100; i++)
        ASSERT_EQ(AV_OK, av_ttl_set(t, i, 10 + i));
    ASSERT_EQ(0u, (unsigned) av_ttl_count_due(t, 9, NULL));
    ASSERT_EQ(1u, (unsigned) av_ttl_count_due(t, 10, NULL));
    ASSERT_EQ(51u, (unsigned) av_ttl_count_due(t, 60, NULL));
    ASSERT_EQ(AV_OK, av_ttl_set(t, 0, 500)); /* extended: the old entry is stale */
    av_ttl_remove(t, 1);
    ASSERT_EQ(49u, (unsigned) av_ttl_count_due(t, 60, NULL));
    roaring_bitmap_t *in = roaring_bitmap_from_range(2, 12, 1);
    ASSERT_EQ(10u, (unsigned) av_ttl_count_due(t, 60, in));
    roaring_bitmap_free(in);
    av_ttl_destroy(t);
    PASS();
}

/* ---- av_hnsw_index: background builds, concurrent searches, persistence, band drop (TSan) ---- */

#define IX_N 1500

static uint64_t ix_expiry(void *ctx, av_docid id)
{
    (void) ctx;
    return (id % 2) ? 150 : 350; /* bands 1 and 3 of 100 blocks */
}

typedef struct {
    av_hnsw_index *ix;
    _Atomic bool stop;
    _Atomic size_t searches;
    _Atomic bool failed;
} reader_ctx;

static void *reader_main(void *p)
{
    reader_ctx *r = p;
    Vector *q = vec3(0.3f, -0.2f, 0.1f);
    while (!atomic_load(&r->stop)) {
        av_docid *ids = NULL;
        size_t n = 0, cap = 0;
        av_err e;
        av_err_clear(&e);
        if (av_hnsw_index_search(r->ix, q, NULL, 40, 1, 2000, 10, &ids, &n, &cap, &e) != AV_OK)
            atomic_store(&r->failed, true);
        free(ids);
        atomic_fetch_add(&r->searches, 1);
    }
    free(q);
    return NULL;
}

static av_hnsw_index *ix_open(av_segment *flat, const char *dir, roaring_bitmap_t *live, uint32_t next)
{
    av_hnsw_index_config cfg = {"unit", AV_VT_VECTOR, DIM, av_opclass_find("vector_l2_ops"), 4, 16,
                                flat,   dir,          100};
    av_err e;
    av_err_clear(&e);
    av_hnsw_index *ix = av_hnsw_index_create(&cfg, &e);
    if (ix != NULL && av_hnsw_index_open(ix, live, ix_expiry, NULL, next, 10, &e) != AV_OK) {
        av_hnsw_index_close(ix, false, false, next, 10);
        return NULL;
    }
    return ix;
}

TEST hnsw_index_background_and_persist(void)
{
    char dir[] = "/tmp/av-hnsw-unit-XXXXXX";
    ASSERT(mkdtemp(dir) != NULL);
    av_err e;
    av_err_clear(&e);
    av_segment *flat = NULL;
    ASSERT_EQ(AV_OK, av_segment_create_flat(NULL, AV_VT_VECTOR, DIM, (size_t) 1 << 26, &flat, &e));
    roaring_bitmap_t *live = roaring_bitmap_create();
    for (int i = 0; i < IX_N; i++) {
        Vector *v = vec_i(i);
        ASSERT_EQ(AV_OK, av_segment_insert(flat, (av_docid) i, v, &e));
        roaring_bitmap_add(live, (uint32_t) i);
        free(v);
    }
    av_hnsw_index *ix = ix_open(flat, dir, live, IX_N);
    ASSERT(ix != NULL);
    av_hnsw_band_info bands[8];
    av_hnsw_index_stats st;
    ASSERT_EQ(2u, (unsigned) av_hnsw_index_describe(ix, bands, 8, &st));
    ASSERT_EQ(0u, (unsigned) st.loads);

    reader_ctx rc = {ix, false, 0, false};
    pthread_t thr;
    ASSERT_EQ(0, pthread_create(&thr, NULL, reader_main, &rc));
    /* the applier keeps adding while the worker builds; publishes adopt the builds */
    uint32_t next = IX_N;
    for (int round = 0; round < 2000; round++) {
        if (round < 100) {
            Vector *v = vec_i((int) next);
            (void) av_segment_insert(flat, next, v, &e);
            av_hnsw_index_add(ix, next, v, ix_expiry(NULL, next));
            roaring_bitmap_add(live, next);
            next++;
            free(v);
        }
        av_hnsw_index_after_publish(ix, live, 20, next);
        (void) av_hnsw_index_describe(ix, bands, 8, &st);
        if (st.builds >= 2 && round >= 100)
            break;
        usleep(2000);
    }
    atomic_store(&rc.stop, true);
    pthread_join(thr, NULL);
    ASSERT_FALSE(atomic_load(&rc.failed));
    ASSERT(atomic_load(&rc.searches) > 0);
    ASSERT_EQ(2u, (unsigned) av_hnsw_index_describe(ix, bands, 8, &st));
    ASSERT_EQ(2u, (unsigned) st.builds);
    ASSERT_EQ(IX_N + 100, (int) (bands[0].graph_count + bands[1].graph_count));
    ASSERT_EQ(0u, (unsigned) (bands[0].tail_count + bands[1].tail_count));
    ASSERT_STR_EQ("built", bands[0].source);

    /* persist at close, reopen: both bands load, nothing to build */
    av_hnsw_index_close(ix, true, false, next, 20);
    ix = ix_open(flat, dir, live, next);
    ASSERT(ix != NULL);
    ASSERT_EQ(2u, (unsigned) av_hnsw_index_describe(ix, bands, 8, &st));
    ASSERT_EQ(2u, (unsigned) st.loads);
    ASSERT_EQ(0u, (unsigned) st.builds_running);
    ASSERT_STR_EQ("loaded", bands[0].source);
    ASSERT_STR_EQ("loaded", bands[1].source);

    /* block 199 is band 1's last block: released whole, its file removed */
    av_hnsw_index_after_publish(ix, live, 199, next);
    ASSERT_EQ(1u, (unsigned) av_hnsw_index_describe(ix, bands, 8, &st));
    ASSERT_EQ(3u, (unsigned) bands[0].band);
    ASSERT_EQ(1u, (unsigned) st.bands_dropped);
    char path[512];
    snprintf(path, sizeof path, "%s/%020d.hnsw", dir, 1);
    ASSERT(access(path, F_OK) != 0);
    av_hnsw_index_close(ix, false, true, next, 199); /* remove_files: the directory goes too */
    ASSERT(access(dir, F_OK) != 0);
    roaring_bitmap_free(live);
    av_segment_destroy(flat);
    PASS();
}

/* [hnsw] became a real section: the ARCHITECTURE 16.3 example keys must keep loading. */
TEST config_hnsw_and_write_sections(void)
{
    av_config c;
    av_err e;
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_config_defaults(&c, &e));
    ASSERT_EQ(AV_OK,
              av_config_load_string(&c,
                                    "[hnsw]\nef_search = 40\niterative_scan = \"relaxed_order\"\n"
                                    "max_scan_tuples = 20000\nscan_mem_multiplier = 1.0\nband_blocks = 500\n"
                                    "[write]\ngas_margin_pct = 15\nmax_fee_cap_gwei = 3.5\n",
                                    "unit", &e));
    ASSERT_EQ(500, (int) c.hnsw.band_blocks);
    ASSERT_EQ(1, (int) c.hnsw.persist);
    ASSERT_EQ(15, (int) c.write.gas_margin_pct);
    ASSERT(c.write.max_fee_cap_gwei > 3.4 && c.write.max_fee_cap_gwei < 3.6);
    av_config_free(&c);
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_config_defaults(&c, &e));
    ASSERT_EQ(AV_ERR, av_config_load_string(&c, "[hnsw]\nno_such_key = 1\n", "unit", &e));
    av_config_free(&c);
    av_err_clear(&e);
    ASSERT_EQ(AV_OK, av_config_defaults(&c, &e));
    ASSERT_EQ(AV_ERR, av_config_load_string(&c, "[write]\ngas_margin_pct = 0\n", "unit", &e));
    av_config_free(&c);
    PASS();
}

SUITE(hnsw_file)
{
    RUN_TEST(hnsw_file_round_trip);
    RUN_TEST(hnsw_file_header_checks);
    RUN_TEST(hnsw_file_corruption_sweep);
    RUN_TEST(ttl_count_due);
    RUN_TEST(hnsw_index_background_and_persist);
    RUN_TEST(config_hnsw_and_write_sections);
}
