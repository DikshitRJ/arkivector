/*
 * index/hnsw_index.c: per-expiry-band HNSW graphs with background builds and persistence.
 * See hnsw_index.h.
 */
#include "index/hnsw_index.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "server/metrics.h"
#include "version.h"

#define AV_HNSW_MIN_CAPACITY ((size_t) 65536)
#define MAX_FILE_BYTES       ((uint64_t) 64 << 30)

/* ------------------------------------------------------------------ CRC-32 (IEEE, reflected) */

static uint32_t g_crc_table[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}

uint32_t av_crc32(uint32_t crc, const void *buf, size_t len)
{
    (void) pthread_once(&g_crc_once, crc_init);
    const uint8_t *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = g_crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ metrics */

static av_mfamily *g_m_loads, *g_m_load_fail, *g_m_builds, *g_m_running, *g_m_bands, *g_m_dropped, *g_m_saves,
    *g_m_build_ms;
static pthread_once_t g_metrics_once = PTHREAD_ONCE_INIT;

static void metrics_init(void)
{
    static const char *const l[] = {"collection"};
    static const double bounds[] = {10, 100, 1000, 10000, 60000, 600000};
    g_m_loads = av_metric_register("arkivector_hnsw_graph_loads_total", "HNSW band graphs loaded from disk",
                                   AV_MTYPE_COUNTER, l, 1);
    g_m_load_fail = av_metric_register("arkivector_hnsw_graph_load_failures_total",
                                       "HNSW band files rejected (corrupt or mismatched) and rebuilt",
                                       AV_MTYPE_COUNTER, l, 1);
    g_m_builds = av_metric_register("arkivector_hnsw_rebuilds_total", "Background HNSW band builds adopted",
                                    AV_MTYPE_COUNTER, l, 1);
    g_m_running = av_metric_register("arkivector_hnsw_rebuilds_in_progress", "Background HNSW builds running",
                                     AV_MTYPE_GAUGE, l, 1);
    g_m_bands = av_metric_register("arkivector_hnsw_bands", "HNSW expiry bands", AV_MTYPE_GAUGE, l, 1);
    g_m_dropped = av_metric_register("arkivector_hnsw_bands_dropped_total", "Expired HNSW bands released",
                                     AV_MTYPE_COUNTER, l, 1);
    g_m_saves = av_metric_register("arkivector_hnsw_graph_saves_total", "HNSW band files written",
                                   AV_MTYPE_COUNTER, l, 1);
    g_m_build_ms = av_metric_register_histogram("arkivector_hnsw_build_ms", "Background HNSW band build time",
                                                l, 1, bounds, sizeof bounds / sizeof bounds[0]);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e3 + (double) ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ structures */

typedef struct {
    uint64_t id;
    av_segment *g;
    roaring_bitmap_t *tail;    /* docids of this band that are not in g (scanned exactly) */
    roaring_bitmap_t *members; /* docids whose current expiry is in this band (may include dead ones) */
    bool full;
    bool building;
    bool save_pending;
    const char *source;
    uint64_t mods;  /* graph changes (inserts, swaps) */
    uint64_t saved; /* value of mods at the last successful save */
    double last_save_ms;
} band;

typedef enum { JOB_BUILD, JOB_SAVE } job_kind;

typedef struct job {
    job_kind kind;
    uint64_t band_id;
    uint32_t next_docid;
    uint64_t watermark;
    roaring_bitmap_t *ids; /* BUILD: the snapshot */
    /* BUILD result */
    av_segment *g;
    roaring_bitmap_t *tail;
    bool full;
    bool saved;
    double ms;
    struct job *next;
} job;

struct av_hnsw_index {
    char *name;
    char *dir;
    av_vtype vtype;
    int dim;
    const av_opclass *oc;
    int m;
    int ef_construction;
    const av_segment *flat;
    uint64_t band_blocks;
    int build_delay_ms; /* AV_TEST_HNSW_BUILD_DELAY_MS: integration tests only */

    pthread_rwlock_t lock;
    band **bands;
    size_t n_bands, cap_bands;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    job *pending; /* FIFO */
    job *done;
    pthread_t thr;
    bool thr_started;
    _Atomic bool stop;
    _Atomic bool rebuild_req;

    _Atomic uint64_t n_loads, n_load_fail, n_builds, n_running, n_dropped, n_saves;
};

/* ------------------------------------------------------------------ small helpers */

static size_t capacity_for(size_t n)
{
    size_t cap = 2 * n;
    return cap > AV_HNSW_MIN_CAPACITY ? cap : AV_HNSW_MIN_CAPACITY;
}

static av_status new_graph(const av_hnsw_index *ix, size_t n, av_segment **out, av_err *e)
{
    size_t reserve = av_segment_hnsw_reserve(ix->vtype, ix->dim, ix->m, capacity_for(n));
    return av_segment_create_hnsw(NULL, ix->vtype, ix->dim, ix->oc, ix->m, ix->ef_construction, reserve, out,
                                  e);
}

static uint64_t band_of(const av_hnsw_index *ix, uint64_t expires)
{
    if (expires == 0)
        return AV_HNSW_BAND_NONE;
    if (ix->band_blocks == 0)
        return 0;
    return expires / ix->band_blocks;
}

/* All of band b's entities expired at block: its last block (b+1)*W - 1 <= block. */
static bool band_expired(const av_hnsw_index *ix, uint64_t b, uint64_t block)
{
    if (b == AV_HNSW_BAND_NONE || ix->band_blocks == 0)
        return false;
    uint64_t W = ix->band_blocks;
    if (b >= UINT64_MAX / W - 1)
        return false;
    return (b + 1) * W - 1 <= block;
}

static void band_path(const av_hnsw_index *ix, uint64_t b, char *out, size_t cap)
{
    if (b == AV_HNSW_BAND_NONE)
        snprintf(out, cap, "%s/noexpiry.hnsw", ix->dir);
    else
        snprintf(out, cap, "%s/%020" PRIu64 ".hnsw", ix->dir, b);
}

/* Index of band b in the sorted array, or the insertion point with *found = false. */
static size_t band_find(const av_hnsw_index *ix, uint64_t b, bool *found)
{
    size_t lo = 0, hi = ix->n_bands;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ix->bands[mid]->id < b)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < ix->n_bands && ix->bands[lo]->id == b;
    return lo;
}

static void band_free(band *bd)
{
    if (bd == NULL)
        return;
    av_segment_destroy(bd->g);
    roaring_bitmap_free(bd->tail);
    roaring_bitmap_free(bd->members);
    free(bd);
}

static band *band_new(uint64_t id)
{
    band *bd = calloc(1, sizeof *bd);
    if (bd == NULL)
        return NULL;
    bd->id = id;
    bd->tail = roaring_bitmap_create();
    bd->members = roaring_bitmap_create();
    bd->source = "live";
    if (bd->tail == NULL || bd->members == NULL) {
        band_free(bd);
        return NULL;
    }
    return bd;
}

/* Caller holds the write lock (or is the only thread). Takes ownership of bd. */
static bool band_insert(av_hnsw_index *ix, band *bd)
{
    bool found;
    size_t at = band_find(ix, bd->id, &found);
    if (found)
        return false;
    if (ix->n_bands == ix->cap_bands) {
        size_t nc = ix->cap_bands ? ix->cap_bands * 2 : 8;
        band **nb = realloc(ix->bands, nc * sizeof *nb);
        if (nb == NULL)
            return false;
        ix->bands = nb;
        ix->cap_bands = nc;
    }
    memmove(&ix->bands[at + 1], &ix->bands[at], (ix->n_bands - at) * sizeof *ix->bands);
    ix->bands[at] = bd;
    ix->n_bands++;
    return true;
}

static void graph_put(const av_hnsw_index *ix, av_segment *g, roaring_bitmap_t *tail, bool *full, av_docid id,
                      const void *pg)
{
    av_err he;
    av_err_clear(&he);
    av_status st = *full ? AV_SEAL_NEEDED : av_segment_insert(g, id, pg, &he);
    if (st == AV_SEAL_NEEDED && !*full) {
        *full = true;
        av_log(AV_LOG_WARN, "hnsw",
               "%s: graph full at %zu vectors; new vectors are scanned exactly until the next rebuild",
               ix->name, av_segment_count(g));
    }
    if (st == AV_ERR)
        av_log(AV_LOG_WARN, "hnsw", "%s: docid %u not added to the graph (%s); scanned exactly", ix->name, id,
               he.msg);
    if (st != AV_OK)
        roaring_bitmap_add(tail, id);
}

/* ------------------------------------------------------------------ file format */

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t) (v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t) (v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t) get32(p) | (uint64_t) get32(p + 4) << 32;
}

static void encode_header(uint8_t hdr[AV_HNSW_FILE_HEADER], const void *img, size_t img_len,
                          const av_seg_hnsw_info *info, const char *opclass, uint64_t band_id,
                          uint64_t band_blocks, uint32_t next_docid, uint64_t watermark)
{
    memset(hdr, 0, AV_HNSW_FILE_HEADER);
    static const uint8_t magic[8] = {'A', 'V', 'H', 'N', 'S', 'W', '0', '1'}; /* AV_HNSW_FILE_MAGIC */
    memcpy(hdr, magic, sizeof magic);
    put32(hdr + 8, AV_HNSW_FILE_VERSION);
    put32(hdr + 12, AV_HNSW_FILE_HEADER);
    snprintf((char *) hdr + 16, 32, "%s", AV_PGVECTOR_TAG);
    snprintf((char *) hdr + 48, 32, "%s", opclass != NULL ? opclass : "");
    put32(hdr + 80, (uint32_t) info->type);
    put32(hdr + 84, (uint32_t) info->dim);
    put32(hdr + 88, (uint32_t) info->m);
    put32(hdr + 92, (uint32_t) info->ef_construction);
    put64(hdr + 96, band_id);
    put64(hdr + 104, band_blocks);
    put64(hdr + 112, (uint64_t) info->count);
    put64(hdr + 120, (uint64_t) img_len);
    put32(hdr + 128, info->min_docid);
    put32(hdr + 132, info->max_docid);
    put32(hdr + 136, next_docid);
    put64(hdr + 144, watermark);
    put32(hdr + 248, av_crc32(0, img, img_len));
    put32(hdr + 252, av_crc32(0, hdr, 252));
}

av_status av_hnsw_file_encode(const void *img, size_t img_len, const av_seg_hnsw_info *info,
                              const char *opclass, uint64_t band_id, uint64_t band_blocks,
                              uint32_t next_docid, uint64_t watermark, uint8_t **out, size_t *out_len,
                              av_err *e)
{
    uint8_t *buf = malloc(AV_HNSW_FILE_HEADER + img_len);
    if (buf == NULL) {
        av_err_set(e, "internal", "out of memory encoding a graph file");
        return AV_ERR;
    }
    encode_header(buf, img, img_len, info, opclass, band_id, band_blocks, next_docid, watermark);
    memcpy(buf + AV_HNSW_FILE_HEADER, img, img_len);
    *out = buf;
    *out_len = AV_HNSW_FILE_HEADER + img_len;
    return AV_OK;
}

av_status av_hnsw_file_decode(const uint8_t *buf, size_t len, const av_hnsw_file_expect *ex,
                              const av_opclass *oc, bool check_crc, size_t reserve_bytes, av_segment **out,
                              av_hnsw_file_info *info, av_err *e)
{
    *out = NULL;
    const char *why = NULL;
    if (buf == NULL || len < AV_HNSW_FILE_HEADER || memcmp(buf, AV_HNSW_FILE_MAGIC, 8) != 0)
        why = "not an arkivector graph file";
    else if (get32(buf + 8) != AV_HNSW_FILE_VERSION || get32(buf + 12) != AV_HNSW_FILE_HEADER)
        why = "unsupported file version";
    else if (check_crc && get32(buf + 252) != av_crc32(0, buf, 252))
        why = "header checksum mismatch";
    if (why != NULL) {
        av_err_set(e, "invalid_argument", "%s", why);
        return AV_ERR;
    }
    char tag[33], opc[33];
    memcpy(tag, buf + 16, 32);
    tag[32] = '\0';
    memcpy(opc, buf + 48, 32);
    opc[32] = '\0';
    av_seg_hnsw_info hi;
    memset(&hi, 0, sizeof hi);
    hi.type = (av_vtype) get32(buf + 80);
    hi.dim = (int) get32(buf + 84);
    hi.m = (int) get32(buf + 88);
    hi.ef_construction = (int) get32(buf + 92);
    uint64_t band_id = get64(buf + 96);
    uint64_t band_blocks = get64(buf + 104);
    uint64_t count = get64(buf + 112);
    uint64_t img_len = get64(buf + 120);
    hi.min_docid = get32(buf + 128);
    hi.max_docid = get32(buf + 132);
    uint32_t next_at_save = get32(buf + 136);
    uint64_t watermark = get64(buf + 144);

    if (strcmp(tag, AV_PGVECTOR_TAG) != 0)
        why = "written with another pgvector version";
    else if (strcmp(opc, ex->opclass != NULL ? ex->opclass : "") != 0 || hi.type != ex->vtype ||
             hi.dim != ex->dim)
        why = "column type, dimension or opclass changed";
    else if (hi.m != ex->m || hi.ef_construction != ex->ef_construction)
        why = "m or ef_construction changed";
    else if (band_id != ex->band || band_blocks != ex->band_blocks)
        why = "expiry band layout changed";
    else if (img_len != len - AV_HNSW_FILE_HEADER || img_len > MAX_FILE_BYTES)
        why = "image size does not match the file";
    else if (count > 0 && hi.max_docid >= ex->next_docid)
        why = "graph holds docids the store has not committed";
    else if (check_crc && get32(buf + 248) != av_crc32(0, buf + AV_HNSW_FILE_HEADER, (size_t) img_len))
        why = "image checksum mismatch";
    if (why != NULL) {
        av_err_set(e, "invalid_argument", "%s", why);
        return AV_ERR;
    }
    hi.count = (size_t) count;
    hi.arena_bytes = (size_t) img_len;
    if (count > 0 && hi.max_docid < hi.min_docid) {
        av_err_set(e, "invalid_argument", "docid range inverted");
        return AV_ERR;
    }
    /* restore reads structs from the image: give it 8-byte alignment */
    const uint8_t *img = buf + AV_HNSW_FILE_HEADER;
    void *aligned = NULL;
    if (((uintptr_t) img & 7u) != 0) {
        aligned = malloc(img_len > 0 ? (size_t) img_len : 1);
        if (aligned == NULL) {
            av_err_set(e, "internal", "out of memory");
            return AV_ERR;
        }
        memcpy(aligned, img, (size_t) img_len);
        img = aligned;
    }
    av_seg_hnsw_info expect = hi;
    if (count == 0)
        expect.max_docid = 0;
    av_status st = av_segment_hnsw_restore(img, (size_t) img_len, &expect, oc, reserve_bytes, out, e);
    free(aligned);
    if (st == AV_OK && info != NULL) {
        info->band = band_id;
        info->watermark_block = watermark;
        info->next_docid_at_save = next_at_save;
        info->count = (size_t) count;
    }
    return st;
}

static av_status write_file_atomic(const char *path, const uint8_t *hdr, size_t hdr_len, const void *img,
                                   size_t img_len, av_err *e)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        av_err_set(e, "internal", "open %s: %s", tmp, strerror(errno));
        return AV_ERR;
    }
    const uint8_t *parts[2] = {hdr, img};
    size_t lens[2] = {hdr_len, img_len};
    for (int i = 0; i < 2; i++) {
        size_t off = 0;
        while (off < lens[i]) {
            ssize_t w = write(fd, parts[i] + off, lens[i] - off);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                av_err_set(e, "internal", "write %s: %s", tmp, strerror(errno));
                close(fd);
                unlink(tmp);
                return AV_ERR;
            }
            off += (size_t) w;
        }
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        av_err_set(e, "internal", "fsync %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return AV_ERR;
    }
    if (rename(tmp, path) != 0) {
        av_err_set(e, "internal", "rename %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return AV_ERR;
    }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void) fsync(dfd);
            close(dfd);
        }
    }
    return AV_OK;
}

/* Write graph g of band b. g must not change meanwhile (caller's lock, or g not shared). */
static av_status save_graph(av_hnsw_index *ix, uint64_t b, const av_segment *g, uint32_t next_docid,
                            uint64_t watermark, av_err *e)
{
    void *img = NULL;
    size_t len = 0;
    av_seg_hnsw_info info;
    if (av_segment_hnsw_snapshot(g, &img, &len, &info, e) != AV_OK)
        return AV_ERR;
    uint8_t hdr[AV_HNSW_FILE_HEADER];
    encode_header(hdr, img, len, &info, ix->oc != NULL ? ix->oc->name : "", b, ix->band_blocks, next_docid,
                  watermark);
    char path[1024];
    band_path(ix, b, path, sizeof path);
    av_status st = write_file_atomic(path, hdr, sizeof hdr, img, len, e);
    free(img);
    if (st == AV_OK) {
        atomic_fetch_add(&ix->n_saves, 1);
        av_metric_inc(g_m_saves, (const char *const *) &ix->name, 1);
    }
    return st;
}

/* ------------------------------------------------------------------ worker */

typedef struct {
    const av_hnsw_index *ix;
    av_segment *g;
    roaring_bitmap_t *tail;
    bool full;
    bool aborted;
} build_ctx;

static bool build_cb(uint32_t id, void *p)
{
    build_ctx *b = p;
    if (atomic_load_explicit(&((av_hnsw_index *) b->ix)->stop, memory_order_relaxed)) {
        b->aborted = true;
        return false;
    }
    const void *v = av_segment_get(b->ix->flat, (av_docid) id);
    if (v != NULL)
        graph_put(b->ix, b->g, b->tail, &b->full, (av_docid) id, v);
    return true;
}

static void run_build(av_hnsw_index *ix, job *j)
{
    double t0 = now_ms();
    size_t n = (size_t) roaring_bitmap_get_cardinality(j->ids);
    av_err e;
    av_err_clear(&e);
    if (new_graph(ix, n, &j->g, &e) != AV_OK || (j->tail = roaring_bitmap_create()) == NULL) {
        av_log(AV_LOG_WARN, "hnsw", "%s: background build of band %" PRIu64 " failed: %s", ix->name,
               j->band_id, e.msg[0] ? e.msg : "out of memory");
        av_segment_destroy(j->g);
        j->g = NULL;
        return;
    }
    build_ctx b = {ix, j->g, j->tail, false, false};
    roaring_iterate(j->ids, build_cb, &b);
    for (int waited = 0; !b.aborted && waited < ix->build_delay_ms; waited += 20) {
        if (atomic_load(&ix->stop))
            b.aborted = true;
        else
            usleep(20000);
    }
    if (b.aborted) {
        av_segment_destroy(j->g);
        roaring_bitmap_free(j->tail);
        j->g = NULL;
        j->tail = NULL;
        return;
    }
    j->full = b.full;
    if (ix->dir != NULL) {
        av_err se;
        av_err_clear(&se);
        if (save_graph(ix, j->band_id, j->g, j->next_docid, j->watermark, &se) == AV_OK)
            j->saved = true;
        else
            av_log(AV_LOG_WARN, "hnsw", "%s: saving band %" PRIu64 " failed: %s", ix->name, j->band_id,
                   se.msg);
    }
    j->ms = now_ms() - t0;
}

static void run_save(av_hnsw_index *ix, job *j)
{
    av_err e;
    av_err_clear(&e);
    (void) pthread_rwlock_rdlock(&ix->lock);
    bool found;
    size_t at = band_find(ix, j->band_id, &found);
    band *bd = found ? ix->bands[at] : NULL;
    uint64_t mods = bd != NULL ? bd->mods : 0;
    void *img = NULL;
    size_t len = 0;
    av_seg_hnsw_info info;
    av_status st = AV_ERR;
    if (bd != NULL && bd->g != NULL)
        st = av_segment_hnsw_snapshot(bd->g, &img, &len, &info, &e);
    (void) pthread_rwlock_unlock(&ix->lock);
    if (bd == NULL)
        return;
    if (st == AV_OK) {
        uint8_t hdr[AV_HNSW_FILE_HEADER];
        encode_header(hdr, img, len, &info, ix->oc != NULL ? ix->oc->name : "", j->band_id, ix->band_blocks,
                      j->next_docid, j->watermark);
        char path[1024];
        band_path(ix, j->band_id, path, sizeof path);
        st = write_file_atomic(path, hdr, sizeof hdr, img, len, &e);
        if (st == AV_OK) {
            atomic_fetch_add(&ix->n_saves, 1);
            av_metric_inc(g_m_saves, (const char *const *) &ix->name, 1);
        }
    }
    free(img);
    if (st != AV_OK)
        av_log(AV_LOG_WARN, "hnsw", "%s: checkpoint of band %" PRIu64 " failed: %s", ix->name, j->band_id,
               e.msg);
    (void) pthread_rwlock_wrlock(&ix->lock);
    at = band_find(ix, j->band_id, &found);
    if (found) {
        ix->bands[at]->save_pending = false;
        ix->bands[at]->last_save_ms = now_ms();
        if (st == AV_OK)
            ix->bands[at]->saved = mods;
    }
    (void) pthread_rwlock_unlock(&ix->lock);
}

static void job_free(job *j)
{
    if (j == NULL)
        return;
    roaring_bitmap_free(j->ids);
    roaring_bitmap_free(j->tail);
    av_segment_destroy(j->g);
    free(j);
}

static void *worker_main(void *arg)
{
    av_hnsw_index *ix = arg;
    for (;;) {
        pthread_mutex_lock(&ix->mu);
        while (ix->pending == NULL && !atomic_load(&ix->stop))
            pthread_cond_wait(&ix->cv, &ix->mu);
        if (atomic_load(&ix->stop)) {
            pthread_mutex_unlock(&ix->mu);
            break;
        }
        job *j = ix->pending;
        ix->pending = j->next;
        j->next = NULL;
        pthread_mutex_unlock(&ix->mu);

        if (j->kind == JOB_BUILD) {
            run_build(ix, j);
            pthread_mutex_lock(&ix->mu);
            j->next = ix->done;
            ix->done = j;
            pthread_mutex_unlock(&ix->mu);
        } else {
            run_save(ix, j);
            job_free(j);
        }
    }
    return NULL;
}

static void enqueue(av_hnsw_index *ix, job *j)
{
    pthread_mutex_lock(&ix->mu);
    if (!ix->thr_started) {
        if (pthread_create(&ix->thr, NULL, worker_main, ix) == 0) {
            ix->thr_started = true;
        } else {
            pthread_mutex_unlock(&ix->mu);
            av_log(AV_LOG_ERROR, "hnsw", "%s: cannot start the HNSW worker thread", ix->name);
            job_free(j);
            return;
        }
    }
    job **pp = &ix->pending;
    while (*pp != NULL)
        pp = &(*pp)->next;
    *pp = j;
    pthread_cond_signal(&ix->cv);
    pthread_mutex_unlock(&ix->mu);
}

/* Applier: snapshot (members AND live) of band bd and queue its build. Caller holds no lock on bd
 * other than being the applier (the only writer of members/building). */
static void schedule_build(av_hnsw_index *ix, band *bd, const roaring_bitmap_t *live, uint32_t next_docid,
                           uint64_t watermark)
{
    if (bd->building)
        return;
    job *j = calloc(1, sizeof *j);
    if (j == NULL)
        return;
    j->kind = JOB_BUILD;
    j->band_id = bd->id;
    j->next_docid = next_docid;
    j->watermark = watermark;
    j->ids = live != NULL ? roaring_bitmap_and(bd->members, live) : roaring_bitmap_copy(bd->members);
    if (j->ids == NULL) {
        free(j);
        return;
    }
    (void) pthread_rwlock_wrlock(&ix->lock);
    bd->building = true;
    (void) pthread_rwlock_unlock(&ix->lock);
    uint64_t running = atomic_fetch_add(&ix->n_running, 1) + 1;
    av_metric_set(g_m_running, (const char *const *) &ix->name, (double) running);
    enqueue(ix, j);
}

/* ------------------------------------------------------------------ lifecycle */

av_hnsw_index *av_hnsw_index_create(const av_hnsw_index_config *cfg, av_err *e)
{
    (void) pthread_once(&g_metrics_once, metrics_init);
    av_hnsw_index *ix = calloc(1, sizeof *ix);
    if (ix == NULL) {
        av_err_set(e, "internal", "out of memory");
        return NULL;
    }
    ix->name = strdup(cfg->coll_name != NULL ? cfg->coll_name : "");
    ix->dir = cfg->dir != NULL ? strdup(cfg->dir) : NULL;
    ix->vtype = cfg->vtype;
    ix->dim = cfg->dim;
    ix->oc = cfg->oc;
    ix->m = cfg->m > 0 ? cfg->m : 16;
    ix->ef_construction = cfg->ef_construction > 0 ? cfg->ef_construction : 64;
    ix->flat = cfg->flat;
    ix->band_blocks = cfg->band_blocks;
    const char *delay = getenv("AV_TEST_HNSW_BUILD_DELAY_MS");
    long delay_ms = delay != NULL ? strtol(delay, NULL, 10) : 0;
    ix->build_delay_ms = delay_ms > 0 && delay_ms < 600000 ? (int) delay_ms : 0;
    if (ix->name == NULL || (cfg->dir != NULL && ix->dir == NULL)) {
        free(ix->name);
        free(ix->dir);
        free(ix);
        av_err_set(e, "internal", "out of memory");
        return NULL;
    }
    pthread_rwlockattr_t la;
    (void) pthread_rwlockattr_init(&la);
#ifdef __GLIBC__ /* musl (Alpine image) has no rwlock kinds */
    (void) pthread_rwlockattr_setkind_np(&la, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    (void) pthread_rwlock_init(&ix->lock, &la);
    (void) pthread_rwlockattr_destroy(&la);
    (void) pthread_mutex_init(&ix->mu, NULL);
    (void) pthread_cond_init(&ix->cv, NULL);
    if (ix->dir != NULL) {
        /* mkdir -p */
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", ix->dir);
        for (char *p = tmp + 1; *p != '\0'; p++) {
            if (*p == '/') {
                *p = '\0';
                (void) mkdir(tmp, 0700);
                *p = '/';
            }
        }
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            av_log(AV_LOG_WARN, "hnsw", "%s: cannot create %s (%s): graphs will not be persisted", ix->name,
                   tmp, strerror(errno));
            free(ix->dir);
            ix->dir = NULL;
        }
    }
    return ix;
}

typedef struct {
    av_hnsw_index *ix;
    av_hnsw_expiry_fn expiry;
    void *ctx;
    bool oom;
} part_ctx;

static bool part_cb(uint32_t id, void *p)
{
    part_ctx *pc = p;
    uint64_t b = band_of(pc->ix, pc->expiry(pc->ctx, (av_docid) id));
    bool found;
    size_t at = band_find(pc->ix, b, &found);
    band *bd = found ? pc->ix->bands[at] : NULL;
    if (bd == NULL) {
        bd = band_new(b);
        if (bd == NULL || !band_insert(pc->ix, bd)) {
            band_free(bd);
            pc->oom = true;
            return false;
        }
    }
    roaring_bitmap_add(bd->members, id);
    return true;
}

/* Try the file of band bd. True with bd->g set when it was loaded. */
static bool load_band(av_hnsw_index *ix, band *bd, const roaring_bitmap_t *live, uint32_t next_docid)
{
    char path[1024];
    band_path(ix, bd->id, path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            av_log(AV_LOG_WARN, "hnsw", "%s: cannot open %s (%s); rebuilding the band", ix->name, path,
                   strerror(errno));
        else
            av_log(AV_LOG_INFO, "hnsw", "%s: no graph file for band %s yet; building it in the background",
                   ix->name, path);
        return false;
    }
    struct stat sb;
    uint8_t *data = NULL;
    size_t size = 0;
    const char *why = NULL;
    av_err e;
    av_err_clear(&e);
    if (fstat(fd, &sb) != 0 || sb.st_size < (off_t) AV_HNSW_FILE_HEADER ||
        (uint64_t) sb.st_size > MAX_FILE_BYTES)
        why = "bad file size";
    else if ((data = malloc((size_t) sb.st_size)) == NULL)
        why = "out of memory";
    while (why == NULL && size < (size_t) sb.st_size) {
        ssize_t r = read(fd, data + size, (size_t) sb.st_size - size);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            why = r < 0 ? strerror(errno) : "short read";
        else
            size += (size_t) r;
    }
    close(fd);
    av_segment *g = NULL;
    av_hnsw_file_info fi;
    if (why == NULL) {
        size_t count_hint = (size_t) roaring_bitmap_get_cardinality(bd->members);
        size_t reserve = av_segment_hnsw_reserve(ix->vtype, ix->dim, ix->m, capacity_for(count_hint));
        if (reserve < 2 * size)
            reserve = 2 * size;
        av_hnsw_file_expect ex = {
            ix->vtype, ix->dim,         ix->m,     ix->ef_construction, ix->oc != NULL ? ix->oc->name : "",
            bd->id,    ix->band_blocks, next_docid};
        if (av_hnsw_file_decode(data, size, &ex, ix->oc, true, reserve, &g, &fi, &e) != AV_OK)
            why = e.msg;
    }
    free(data);
    if (why == NULL) {
        size_t bad = 0;
        if (av_segment_hnsw_verify(g, ix->flat, live, &bad, &e) != AV_OK)
            why = e.msg;
        else if (bad > 0) {
            snprintf(e.msg, sizeof e.msg, "%zu live vectors differ from the store", bad);
            why = e.msg;
        }
    }
    if (why != NULL) {
        av_log(AV_LOG_WARN, "hnsw", "%s: graph file %s rejected (%s); rebuilding the band in the background",
               ix->name, path, why);
        av_segment_destroy(g);
        atomic_fetch_add(&ix->n_load_fail, 1);
        av_metric_inc(g_m_load_fail, (const char *const *) &ix->name, 1);
        (void) unlink(path);
        return false;
    }
    bd->g = g;
    bd->source = "loaded";
    atomic_fetch_add(&ix->n_loads, 1);
    av_metric_inc(g_m_loads, (const char *const *) &ix->name, 1);
    av_log(AV_LOG_INFO, "hnsw", "%s: loaded %s (%zu vectors, saved at block %" PRIu64 ")", ix->name, path,
           fi.count, fi.watermark_block);
    return true;
}

typedef struct {
    const av_hnsw_index *ix;
    band *bd;
} add_ctx;

static bool catchup_cb(uint32_t id, void *p)
{
    add_ctx *a = p;
    const void *v = av_segment_get(a->ix->flat, (av_docid) id);
    if (v != NULL)
        graph_put(a->ix, a->bd->g, a->bd->tail, &a->bd->full, (av_docid) id, v);
    return true;
}

static void remove_stale_files(av_hnsw_index *ix, uint64_t as_of_block)
{
    DIR *d = opendir(ix->dir);
    if (d == NULL)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        size_t ln = strlen(nm);
        bool tmp = ln > 4 && strcmp(nm + ln - 4, ".tmp") == 0;
        if (!tmp && (ln < 6 || strcmp(nm + ln - 5, ".hnsw") != 0))
            continue;
        bool keep = false;
        if (!tmp) {
            uint64_t b = AV_HNSW_BAND_NONE;
            bool parsed = strcmp(nm, "noexpiry.hnsw") == 0;
            if (!parsed) {
                char *end = NULL;
                unsigned long long v = strtoull(nm, &end, 10);
                parsed = end != NULL && strcmp(end, ".hnsw") == 0;
                b = (uint64_t) v;
            }
            bool found = false;
            if (parsed)
                (void) band_find(ix, b, &found);
            keep = found && !band_expired(ix, b, as_of_block);
        }
        if (!keep) {
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", ix->dir, nm);
            (void) unlink(path);
        }
    }
    closedir(d);
}

av_status av_hnsw_index_open(av_hnsw_index *ix, const roaring_bitmap_t *live, av_hnsw_expiry_fn expiry,
                             void *ctx, uint32_t next_docid, uint64_t as_of_block, av_err *e)
{
    part_ctx pc = {ix, expiry, ctx, false};
    if (live != NULL)
        roaring_iterate(live, part_cb, &pc);
    if (pc.oom) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    for (size_t i = 0; i < ix->n_bands; i++) {
        band *bd = ix->bands[i];
        if (ix->dir != NULL && load_band(ix, bd, live, next_docid)) {
            roaring_bitmap_t *have = av_segment_docids(bd->g);
            roaring_bitmap_t *delta = have != NULL ? roaring_bitmap_andnot(bd->members, have) : NULL;
            roaring_bitmap_free(have);
            if (delta == NULL) {
                av_err_set(e, "internal", "out of memory");
                return AV_ERR;
            }
            size_t nd = (size_t) roaring_bitmap_get_cardinality(delta);
            size_t gn = av_segment_count(bd->g);
            size_t limit = gn / 10 > 1024 ? gn / 10 : 1024;
            if (nd <= limit) {
                add_ctx a = {ix, bd};
                roaring_iterate(delta, catchup_cb, &a);
                bd->mods = nd;
                bd->saved = 0;
                if (nd > 0)
                    av_log(AV_LOG_INFO, "hnsw",
                           "%s: band %" PRIu64 ": indexed %zu vectors added since the save", ix->name, bd->id,
                           nd);
            } else {
                /* a large delta: scan it exactly and rebuild the band in the background */
                roaring_bitmap_or_inplace(bd->tail, delta);
                schedule_build(ix, bd, live, next_docid, as_of_block);
            }
            roaring_bitmap_free(delta);
            continue;
        }
        if (new_graph(ix, 0, &bd->g, e) != AV_OK)
            return AV_ERR;
        roaring_bitmap_or_inplace(bd->tail, bd->members); /* exact until the build is adopted */
        bd->source = "pending";
        schedule_build(ix, bd, live, next_docid, as_of_block);
    }
    if (ix->dir != NULL)
        remove_stale_files(ix, as_of_block);
    av_metric_set(g_m_bands, (const char *const *) &ix->name, (double) ix->n_bands);
    return AV_OK;
}

/* Applier: band for b, created (empty graph) when missing. Takes the write lock to insert. */
static band *band_get_or_create(av_hnsw_index *ix, uint64_t b)
{
    bool found;
    size_t at =
        band_find(ix, b, &found); /* the applier is the only writer: reading without the lock is safe */
    if (found)
        return ix->bands[at];
    band *bd = band_new(b);
    av_err e;
    av_err_clear(&e);
    if (bd == NULL || new_graph(ix, 0, &bd->g, &e) != AV_OK) {
        band_free(bd);
        return NULL;
    }
    (void) pthread_rwlock_wrlock(&ix->lock);
    bool ok = band_insert(ix, bd);
    (void) pthread_rwlock_unlock(&ix->lock);
    if (!ok) {
        band_free(bd);
        return NULL;
    }
    av_metric_set(g_m_bands, (const char *const *) &ix->name, (double) ix->n_bands);
    return bd;
}

void av_hnsw_index_add(av_hnsw_index *ix, av_docid id, const void *pg, uint64_t expires_at)
{
    if (ix == NULL)
        return;
    band *bd = band_get_or_create(ix, band_of(ix, expires_at));
    if (bd == NULL) {
        av_log(AV_LOG_WARN, "hnsw", "%s: out of memory creating an expiry band; docid %u not indexed",
               ix->name, id);
        return;
    }
    (void) pthread_rwlock_wrlock(&ix->lock);
    roaring_bitmap_add(bd->members, id);
    graph_put(ix, bd->g, bd->tail, &bd->full, id, pg);
    bd->mods++;
    (void) pthread_rwlock_unlock(&ix->lock);
}

void av_hnsw_index_expiry_changed(av_hnsw_index *ix, av_docid id, uint64_t old_expires, uint64_t new_expires)
{
    if (ix == NULL)
        return;
    uint64_t ob = band_of(ix, old_expires), nb = band_of(ix, new_expires);
    if (ob == nb)
        return;
    const void *pg = av_segment_get(ix->flat, id);
    if (pg == NULL)
        return;
    bool found;
    size_t at = band_find(ix, ob, &found);
    if (found) {
        (void) pthread_rwlock_wrlock(&ix->lock);
        roaring_bitmap_remove(ix->bands[at]->members, id);
        roaring_bitmap_remove(ix->bands[at]->tail, id);
        (void) pthread_rwlock_unlock(&ix->lock);
    }
    band *bd = band_get_or_create(ix, nb);
    if (bd == NULL)
        return;
    (void) pthread_rwlock_wrlock(&ix->lock);
    roaring_bitmap_add(bd->members, id);
    if (av_segment_get(bd->g, id) == NULL && !roaring_bitmap_contains(bd->tail, id)) {
        graph_put(ix, bd->g, bd->tail, &bd->full, id, pg);
        bd->mods++;
    }
    (void) pthread_rwlock_unlock(&ix->lock);
}

/* Adopt finished builds: catch up with docids added since the snapshot, then swap. */
static void adopt_builds(av_hnsw_index *ix, const roaring_bitmap_t *live)
{
    pthread_mutex_lock(&ix->mu);
    job *list = ix->done;
    ix->done = NULL;
    pthread_mutex_unlock(&ix->mu);
    while (list != NULL) {
        job *j = list;
        list = j->next;
        uint64_t running = atomic_fetch_sub(&ix->n_running, 1) - 1;
        av_metric_set(g_m_running, (const char *const *) &ix->name, (double) running);
        bool found;
        size_t at = band_find(ix, j->band_id, &found);
        if (!found || j->g == NULL) {
            if (found) {
                (void) pthread_rwlock_wrlock(&ix->lock);
                ix->bands[at]->building = false;
                (void) pthread_rwlock_unlock(&ix->lock);
            }
            job_free(j);
            continue;
        }
        band *bd = ix->bands[at];
        roaring_bitmap_t *now =
            live != NULL ? roaring_bitmap_and(bd->members, live) : roaring_bitmap_copy(bd->members);
        roaring_bitmap_t *delta = now != NULL ? roaring_bitmap_andnot(now, j->ids) : NULL;
        roaring_bitmap_free(now);
        size_t nd = 0;
        if (delta != NULL) {
            band tmp = {.g = j->g, .tail = j->tail, .full = j->full};
            add_ctx a = {ix, &tmp};
            roaring_iterate(delta, catchup_cb, &a);
            j->full = tmp.full;
            nd = (size_t) roaring_bitmap_get_cardinality(delta);
            roaring_bitmap_free(delta);
        }
        (void) pthread_rwlock_wrlock(&ix->lock);
        av_segment *old_g = bd->g;
        roaring_bitmap_t *old_tail = bd->tail;
        bd->g = j->g;
        bd->tail = j->tail;
        bd->full = j->full;
        bd->building = false;
        bd->source = "built";
        bd->mods = nd + 1;
        bd->saved = j->saved ? 1 : 0;
        bd->last_save_ms = now_ms();
        /* members: the snapshot plus what came after; dead docids leave with the rebuild */
        roaring_bitmap_t *nm = live != NULL ? roaring_bitmap_and(bd->members, live) : NULL;
        if (nm != NULL) {
            roaring_bitmap_free(bd->members);
            bd->members = nm;
        }
        (void) pthread_rwlock_unlock(&ix->lock);
        j->g = NULL;
        j->tail = NULL;
        av_segment_destroy(old_g);
        roaring_bitmap_free(old_tail);
        atomic_fetch_add(&ix->n_builds, 1);
        av_metric_inc(g_m_builds, (const char *const *) &ix->name, 1);
        av_metric_observe(g_m_build_ms, (const char *const *) &ix->name, j->ms);
        av_log(AV_LOG_INFO, "hnsw",
               "%s: band %" PRIu64
               " graph rebuilt in the background over %zu vectors in %.0f ms (+%zu caught up)",
               ix->name, j->band_id, (size_t) roaring_bitmap_get_cardinality(j->ids), j->ms, nd);
        job_free(j);
    }
}

void av_hnsw_index_after_publish(av_hnsw_index *ix, const roaring_bitmap_t *live, uint64_t block,
                                 uint32_t next_docid)
{
    if (ix == NULL)
        return;
    /* 1. expired bands: released whole */
    band **drop = NULL;
    size_t n_drop = 0;
    for (size_t i = 0; i < ix->n_bands; i++)
        if (band_expired(ix, ix->bands[i]->id, block))
            n_drop++;
    if (n_drop > 0 && (drop = malloc(n_drop * sizeof *drop)) != NULL) {
        size_t k = 0, w = 0;
        (void) pthread_rwlock_wrlock(&ix->lock);
        for (size_t i = 0; i < ix->n_bands; i++) {
            if (band_expired(ix, ix->bands[i]->id, block) && k < n_drop)
                drop[k++] = ix->bands[i];
            else
                ix->bands[w++] = ix->bands[i];
        }
        ix->n_bands = w;
        (void) pthread_rwlock_unlock(&ix->lock);
        for (size_t i = 0; i < k; i++) {
            if (ix->dir != NULL) {
                char path[1024];
                band_path(ix, drop[i]->id, path, sizeof path);
                (void) unlink(path);
            }
            av_log(AV_LOG_INFO, "hnsw",
                   "%s: expiry band %" PRIu64 " expired at block %" PRIu64 ": released %zu graph vectors",
                   ix->name, drop[i]->id, block, av_segment_count(drop[i]->g));
            band_free(drop[i]);
        }
        free(drop);
        atomic_fetch_add(&ix->n_dropped, k);
        av_metric_inc(g_m_dropped, (const char *const *) &ix->name, (double) k);
        av_metric_set(g_m_bands, (const char *const *) &ix->name, (double) ix->n_bands);
    }

    /* 2. finished background builds */
    adopt_builds(ix, live);

    /* 3. requested rebuilds, rebuilds (large exact tail or dead share) and checkpoints */
    if (atomic_exchange(&ix->rebuild_req, false))
        for (size_t i = 0; i < ix->n_bands; i++)
            schedule_build(ix, ix->bands[i], live, next_docid, block);
    double t = now_ms();
    for (size_t i = 0; i < ix->n_bands; i++) {
        band *bd = ix->bands[i];
        if (bd->building)
            continue;
        size_t graph_n = av_segment_count(bd->g);
        size_t tail_n = (size_t) roaring_bitmap_get_cardinality(bd->tail);
        size_t live_n = live != NULL ? (size_t) roaring_bitmap_and_cardinality(bd->members, live) : 0;
        size_t tail_live = live != NULL ? (size_t) roaring_bitmap_and_cardinality(bd->tail, live) : 0;
        size_t live_in_graph = live_n > tail_live ? live_n - tail_live : 0;
        size_t dead_n = graph_n > live_in_graph ? graph_n - live_in_graph : 0;
        size_t tail_limit = graph_n / 10 > 1024 ? graph_n / 10 : 1024;
        bool tail_big = (bd->full || graph_n == 0) && tail_live > 0 && (graph_n == 0 || tail_n > tail_limit);
        bool dead_big = graph_n >= 1024 && (double) dead_n > 0.30 * (double) graph_n;
        if (tail_big || dead_big) {
            schedule_build(ix, bd, live, next_docid, block);
            continue;
        }
        if (ix->dir == NULL)
            continue;
        uint64_t every = graph_n / 20 > 1000 ? graph_n / 20 : 1000;
        bool want = false;
        (void) pthread_rwlock_wrlock(
            &ix->lock); /* saved / save_pending / last_save_ms are shared with the worker */
        if (!bd->save_pending && bd->mods - bd->saved >= every && t - bd->last_save_ms >= 30000.0) {
            bd->save_pending = true;
            want = true;
        }
        (void) pthread_rwlock_unlock(&ix->lock);
        if (want) {
            job *j = calloc(1, sizeof *j);
            if (j != NULL) {
                j->kind = JOB_SAVE;
                j->band_id = bd->id;
                j->next_docid = next_docid;
                j->watermark = block;
                enqueue(ix, j);
            } else {
                (void) pthread_rwlock_wrlock(&ix->lock);
                bd->save_pending = false;
                (void) pthread_rwlock_unlock(&ix->lock);
            }
        }
    }
}

void av_hnsw_index_request_rebuild(av_hnsw_index *ix)
{
    if (ix != NULL)
        atomic_store(&ix->rebuild_req, true);
}

static av_status push_id(av_docid **ids, size_t *n, size_t *cap, av_docid id)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        av_docid *ni = realloc(*ids, nc * sizeof *ni);
        if (ni == NULL)
            return AV_ERR;
        *ids = ni;
        *cap = nc;
    }
    (*ids)[(*n)++] = id;
    return AV_OK;
}

typedef struct {
    av_docid **ids;
    size_t *n, *cap;
    bool ok;
} push_ctx;

static bool push_cb(uint32_t id, void *p)
{
    push_ctx *pc = p;
    pc->ok = push_id(pc->ids, pc->n, pc->cap, (av_docid) id) == AV_OK;
    return pc->ok;
}

av_status av_hnsw_index_search(av_hnsw_index *ix, const void *query, const roaring_bitmap_t *cand,
                               int ef_search, int iterative_mode, int max_scan_tuples, size_t limit,
                               av_docid **ids, size_t *n_ids, size_t *cap_ids, av_err *e)
{
    av_hit *hits = malloc((limit > 0 ? limit : 1) * sizeof *hits);
    if (hits == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    av_status st = AV_OK;
    (void) pthread_rwlock_rdlock(&ix->lock);
    for (size_t b = 0; b < ix->n_bands && st == AV_OK; b++) {
        band *bd = ix->bands[b];
        if (cand != NULL && !roaring_bitmap_intersect(bd->members, cand) &&
            !roaring_bitmap_intersect(bd->tail, cand))
            continue; /* nothing of this band can match: skip the graph walk */
        if (av_segment_count(bd->g) > 0) {
            size_t ng = 0;
            st = av_segment_search_hnsw(bd->g, query, ef_search, iterative_mode, max_scan_tuples, cand, 0,
                                        limit, hits, &ng, e);
            for (size_t i = 0; st == AV_OK && i < ng && i < limit; i++)
                if (push_id(ids, n_ids, cap_ids, hits[i].id) != AV_OK) {
                    av_err_set(e, "internal", "out of memory");
                    st = AV_ERR;
                }
        }
        if (st == AV_OK && !roaring_bitmap_is_empty(bd->tail)) {
            roaring_bitmap_t *t =
                cand != NULL ? roaring_bitmap_and(bd->tail, cand) : roaring_bitmap_copy(bd->tail);
            if (t == NULL) {
                av_err_set(e, "internal", "out of memory");
                st = AV_ERR;
                break;
            }
            push_ctx pc = {ids, n_ids, cap_ids, true};
            roaring_iterate(t, push_cb, &pc);
            roaring_bitmap_free(t);
            if (!pc.ok) {
                av_err_set(e, "internal", "out of memory");
                st = AV_ERR;
            }
        }
    }
    (void) pthread_rwlock_unlock(&ix->lock);
    free(hits);
    return st;
}

size_t av_hnsw_index_describe(av_hnsw_index *ix, av_hnsw_band_info *out, size_t max, av_hnsw_index_stats *st)
{
    (void) pthread_rwlock_rdlock(&ix->lock);
    size_t n = ix->n_bands;
    for (size_t i = 0; i < n && i < max; i++) {
        band *bd = ix->bands[i];
        out[i].band = bd->id;
        out[i].graph_count = av_segment_count(bd->g);
        out[i].tail_count = (size_t) roaring_bitmap_get_cardinality(bd->tail);
        out[i].source = bd->source;
        out[i].building = bd->building;
    }
    (void) pthread_rwlock_unlock(&ix->lock);
    if (st != NULL) {
        st->bands = n;
        st->loads = atomic_load(&ix->n_loads);
        st->load_failures = atomic_load(&ix->n_load_fail);
        st->builds = atomic_load(&ix->n_builds);
        st->builds_running = atomic_load(&ix->n_running);
        st->bands_dropped = atomic_load(&ix->n_dropped);
        st->saves = atomic_load(&ix->n_saves);
    }
    return n;
}

size_t av_hnsw_index_band_count(av_hnsw_index *ix)
{
    (void) pthread_rwlock_rdlock(&ix->lock);
    size_t n = ix->n_bands;
    (void) pthread_rwlock_unlock(&ix->lock);
    return n;
}

void av_hnsw_index_close(av_hnsw_index *ix, bool persist, bool remove_files, uint32_t next_docid,
                         uint64_t as_of_block)
{
    if (ix == NULL)
        return;
    pthread_mutex_lock(&ix->mu);
    atomic_store(&ix->stop, true);
    pthread_cond_broadcast(&ix->cv);
    bool started = ix->thr_started;
    pthread_mutex_unlock(&ix->mu);
    if (started)
        (void) pthread_join(ix->thr, NULL);
    for (job *j = ix->pending; j != NULL;) {
        job *nx = j->next;
        job_free(j);
        j = nx;
    }
    for (job *j = ix->done; j != NULL;) {
        job *nx = j->next;
        job_free(j);
        j = nx;
    }
    ix->pending = ix->done = NULL;
    av_metric_set(g_m_running, (const char *const *) &ix->name, 0);
    for (size_t i = 0; i < ix->n_bands; i++) {
        band *bd = ix->bands[i];
        if (ix->dir != NULL && remove_files) {
            char path[1024];
            band_path(ix, bd->id, path, sizeof path);
            (void) unlink(path);
        } else if (ix->dir != NULL && persist && bd->g != NULL && av_segment_count(bd->g) > 0 &&
                   bd->mods != bd->saved && !band_expired(ix, bd->id, as_of_block)) {
            av_err e;
            av_err_clear(&e);
            if (save_graph(ix, bd->id, bd->g, next_docid, as_of_block, &e) != AV_OK)
                av_log(AV_LOG_WARN, "hnsw", "%s: saving band %" PRIu64 " at shutdown failed: %s", ix->name,
                       bd->id, e.msg);
        }
        band_free(bd);
    }
    if (ix->dir != NULL && remove_files)
        (void) rmdir(ix->dir);
    free(ix->bands);
    (void) pthread_rwlock_destroy(&ix->lock);
    (void) pthread_mutex_destroy(&ix->mu);
    (void) pthread_cond_destroy(&ix->cv);
    free(ix->name);
    free(ix->dir);
    free(ix);
}
