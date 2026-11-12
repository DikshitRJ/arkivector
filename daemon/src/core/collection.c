/*
 * core/collection.c: the collection runtime. See collection.h for the threading model.
 *
 * Applier calls stage everything a reader could observe (docmap, postings) in `ops` and
 * edit only private state; av_collection_publish applies `ops` under the write lock.
 */
#define _GNU_SOURCE
#include "core/collection.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sha3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "codec/vectorbytes.h"
#include "index/live.h"
#include "store/checkpoint.h"
#include "store/schema.h"
#include "sync/ttl.h"

typedef enum { ST_ASSIGN, ST_UNMAP, ST_META, ST_ATTR_ADD, ST_ATTR_DEL } st_kind;

typedef struct {
    st_kind kind;
    av_docid id;
    uint8_t key[32];
    uint64_t updated, expires;
    char name[AV_ATTR_NAME_MAX + 1];
    uint8_t type;
    const uint8_t *val; /* pend arena */
    size_t len;
} st_op;

typedef struct {
    char name[AV_ATTR_NAME_MAX + 1];
    uint8_t type;
    const uint8_t *val;
    size_t len;
} ixa; /* one indexable attribute */

struct av_collection {
    av_coldef def;
    av_store *store;
    char seg_path[512];
    bool has_seg_path;
    bool dropped;
    _Atomic int refs;

    pthread_rwlock_t lock; /* publish lock */
    pthread_mutex_t wait_mu;
    pthread_cond_t wait_cv;
    _Atomic uint64_t as_of;
    _Atomic size_t pub_count;
    _Atomic bool bootstrapping;

    /* reader-visible, mutated only in publish under the write lock */
    av_docmap *docmap;
    av_attr_index *ix;
    av_live *live;
    av_segment *seg;
    av_ttl *ttl;

    /*
     * HNSW side index (def.method == AV_INDEX_HNSW). The flat segment stays the source of truth
     * (raw vectors, exact scans, distance recomputation); the graph indexes the same docids.
     * hnsw_lock guards hnsw, hnsw_tail and hnsw_full: the applier takes it exclusively per graph
     * insert and for the swap after a rebuild, readers take it shared for one search. Lock order:
     * collection read lock, then hnsw_lock. hnsw_tail holds docids stored in the flat segment but
     * not in the graph (graph full, zero-norm cosine vectors, insert errors): searches scan it exactly.
     */
    pthread_rwlock_t hnsw_lock;
    av_segment *hnsw;
    roaring_bitmap_t *hnsw_tail;
    bool hnsw_full;

    /* applier-private */
    uint32_t next_docid;
    av_arena *scratch;
    av_arena *pend;
    st_op *ops;
    size_t n_ops, cap_ops;
};

/* ------------------------------------------------------------------ helpers */

static av_status oom(av_err *e)
{
    av_err_set(e, "internal", "out of memory");
    return AV_ERR;
}

static const av_attr *find_attr(const av_attr *attrs, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(attrs[i].name, name) == 0)
            return &attrs[i];
    return NULL;
}

static void addr_word(uint8_t out[32], const uint8_t a[20])
{
    memset(out, 0, 12);
    memcpy(out + 12, a, 20);
}

static void u64_word(uint8_t out[32], uint64_t v)
{
    memset(out, 0, 32);
    for (int i = 0; i < 8; i++)
        out[31 - i] = (uint8_t) (v >> (8 * i));
}

static bool ixa_equal(const ixa *a, const ixa *b)
{
    return a->type == b->type && a->len == b->len && strcmp(a->name, b->name) == 0 &&
           (a->len == 0 || memcmp(a->val, b->val, a->len) == 0);
}

/* ------------------------------------------------------------------ staging */

static av_status stage_push(av_collection *c, const st_op *op, av_err *e)
{
    if (c->n_ops == c->cap_ops) {
        size_t nc = c->cap_ops ? c->cap_ops * 2 : 256;
        st_op *nb = realloc(c->ops, nc * sizeof *nb);
        if (nb == NULL)
            return oom(e);
        c->ops = nb;
        c->cap_ops = nc;
    }
    c->ops[c->n_ops++] = *op;
    return AV_OK;
}

static av_status stage_attr(av_collection *c, st_kind kind, av_docid id, const ixa *a, av_err *e)
{
    st_op op;
    memset(&op, 0, sizeof op);
    op.kind = kind;
    op.id = id;
    memcpy(op.name, a->name, sizeof op.name);
    op.type = a->type;
    op.len = a->len;
    uint8_t *copy = av_arena_alloc(c->pend, a->len ? a->len : 1, 8);
    if (copy == NULL)
        return oom(e);
    if (a->len > 0)
        memcpy(copy, a->val, a->len);
    op.val = copy;
    return stage_push(c, &op, e);
}

static av_status stage_diff(av_collection *c, const ixa *ol, size_t no, av_docid oid, const ixa *nl,
                            size_t nn, av_docid nid, bool same_doc, av_err *e)
{
    for (size_t i = 0; i < no; i++) {
        bool keep = false;
        for (size_t j = 0; same_doc && j < nn && !keep; j++)
            keep = ixa_equal(&ol[i], &nl[j]);
        if (!keep && stage_attr(c, ST_ATTR_DEL, oid, &ol[i], e) != AV_OK)
            return AV_ERR;
    }
    for (size_t j = 0; j < nn; j++) {
        bool have = false;
        for (size_t i = 0; same_doc && i < no && !have; i++)
            have = ixa_equal(&ol[i], &nl[j]);
        if (!have && stage_attr(c, ST_ATTR_ADD, nid, &nl[j], e) != AV_OK)
            return AV_ERR;
    }
    return AV_OK;
}

static void stage_reset(av_collection *c)
{
    c->n_ops = 0;
    av_arena_reset(c->pend);
}

/* Which attributes are value-indexed (av.vec / av.doc are not; lite mode keeps only the tag and id). */
static bool attr_indexed(const av_collection *c, const char *name)
{
    if (strcmp(name, AV_ATTR_VEC) == 0 || strcmp(name, AV_ATTR_DOC) == 0)
        return false;
    if (c->def.mode == AV_MODE_LITE)
        return strcmp(name, AV_ATTR_SCHEMA) == 0 || strcmp(name, AV_ATTR_ID) == 0;
    return true;
}

static av_status build_ixa(const av_collection *c, const av_attr *attrs, size_t n, const uint8_t owner[20],
                           const uint8_t creator[20], uint64_t expires, av_arena *a, ixa **out, size_t *nout,
                           av_err *e)
{
    ixa *l = av_arena_alloc(a, (n + 3) * sizeof *l, 8);
    uint8_t *words = av_arena_alloc(a, 96, 8);
    if (l == NULL || words == NULL)
        return oom(e);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!attr_indexed(c, attrs[i].name))
            continue;
        memcpy(l[k].name, attrs[i].name, sizeof l[k].name);
        l[k].type = attrs[i].type;
        l[k].val = attrs[i].val;
        l[k].len = attrs[i].len;
        k++;
    }
    addr_word(words, owner);
    addr_word(words + 32, creator);
    u64_word(words + 64, expires);
    (void) snprintf(l[k].name, sizeof l[k].name, "$owner");
    l[k].type = AV_AT_ADDR;
    l[k].val = words;
    l[k++].len = 32;
    (void) snprintf(l[k].name, sizeof l[k].name, "$creator");
    l[k].type = AV_AT_ADDR;
    l[k].val = words + 32;
    l[k++].len = 32;
    (void) snprintf(l[k].name, sizeof l[k].name, "$expiresAt");
    l[k].type = AV_AT_U64;
    l[k].val = words + 64;
    l[k++].len = 32;
    *out = l;
    *nout = k;
    return AV_OK;
}

/* ---------------------------------------------------------------- lifecycle */

static void *fatal_oom(void)
{
    av_log(AV_LOG_ERROR, "collection", "out of memory while publishing; cannot continue");
    abort();
}

/* ------------------------------------------------------------ HNSW index */

av_status av_collection_rebuild_hnsw(av_collection *c, av_err *e);

#define AV_HNSW_MIN_CAPACITY ((size_t) 65536)

/* Graph capacity for a (re)build over n vectors: room to double before the tail grows. */
static size_t hnsw_capacity(size_t n)
{
    size_t cap = 2 * n;
    return cap > AV_HNSW_MIN_CAPACITY ? cap : AV_HNSW_MIN_CAPACITY;
}

typedef struct {
    av_collection *c;
    av_segment *seg;
    roaring_bitmap_t *tail;
    bool full;
} hnsw_build_ctx;

static void hnsw_put(av_segment *g, roaring_bitmap_t *tail, bool *full, av_docid id, const void *pg,
                     const char *coll)
{
    av_err he;
    av_err_clear(&he);
    av_status st = *full ? AV_SEAL_NEEDED : av_segment_insert(g, id, pg, &he);
    if (st == AV_SEAL_NEEDED && !*full) {
        *full = true;
        av_log(AV_LOG_WARN, "collection",
               "%s: hnsw graph full at %zu vectors; new vectors are scanned exactly "
               "until the next rebuild",
               coll, av_segment_count(g));
    }
    if (st == AV_ERR)
        av_log(AV_LOG_WARN, "collection", "%s: docid %u not added to the hnsw graph (%s); scanned exactly",
               coll, id, he.msg);
    if (st != AV_OK)
        roaring_bitmap_add(tail, id);
}

static bool hnsw_build_cb(uint32_t id, void *p)
{
    hnsw_build_ctx *b = p;
    const void *v = av_segment_get(b->c->seg, (av_docid) id);
    if (v != NULL)
        hnsw_put(b->seg, b->tail, &b->full, (av_docid) id, v, b->c->def.name);
    return true;
}

/* Build a fresh graph over ids (vectors read from the flat segment). Applier thread only. */
static av_status hnsw_build(av_collection *c, const roaring_bitmap_t *ids, av_segment **seg_out,
                            roaring_bitmap_t **tail_out, bool *full_out, av_err *e)
{
    size_t n = ids != NULL ? (size_t) roaring_bitmap_get_cardinality(ids) : 0;
    int m = c->def.m > 0 ? c->def.m : 16;
    int efc = c->def.ef_construction > 0 ? c->def.ef_construction : 64;
    size_t reserve = av_segment_hnsw_reserve(c->def.vtype, (int) c->def.dimensions, m, hnsw_capacity(n));
    av_segment *g = NULL;
    if (av_segment_create_hnsw(NULL, c->def.vtype, (int) c->def.dimensions, c->def.opclass, m, efc, reserve,
                               &g, e) != AV_OK)
        return AV_ERR;
    roaring_bitmap_t *tail = roaring_bitmap_create();
    if (tail == NULL) {
        av_segment_destroy(g);
        return oom(e);
    }
    hnsw_build_ctx b = {c, g, tail, false};
    if (ids != NULL)
        roaring_iterate(ids, hnsw_build_cb, &b);
    *seg_out = g;
    *tail_out = tail;
    *full_out = b.full;
    return AV_OK;
}

/* Applier: index a docid that was just stored in the flat segment. Never fails the batch. */
static void hnsw_add(av_collection *c, av_docid id, const void *pg)
{
    if (c->hnsw == NULL)
        return;
    (void) pthread_rwlock_wrlock(&c->hnsw_lock);
    hnsw_put(c->hnsw, c->hnsw_tail, &c->hnsw_full, id, pg, c->def.name);
    (void) pthread_rwlock_unlock(&c->hnsw_lock);
}

/*
 * Applier, after publish: rebuild the graph from the live docids when the exact tail or the dead
 * share got large (updates and deletes leave dead elements in the graph). Searches keep using the
 * old graph during the build; the swap takes hnsw_lock exclusively for a pointer exchange.
 */
static void hnsw_maybe_rebuild(av_collection *c)
{
    if (c->hnsw == NULL)
        return;
    size_t graph_n = av_segment_count(c->hnsw);
    size_t tail_n = (size_t) roaring_bitmap_get_cardinality(c->hnsw_tail);
    size_t live_n = atomic_load_explicit(&c->pub_count, memory_order_acquire);
    /* the tail also holds dead docids (deleted, discarded batches): count only its live part */
    size_t tail_live = (size_t) roaring_bitmap_and_cardinality(c->hnsw_tail, av_live_current(c->live));
    size_t live_in_graph = live_n > tail_live ? live_n - tail_live : 0;
    size_t dead_n = graph_n > live_in_graph ? graph_n - live_in_graph : 0;
    size_t tail_limit = graph_n / 10 > 1024 ? graph_n / 10 : 1024;
    bool tail_big = c->hnsw_full && tail_n > tail_limit;
    bool dead_big = graph_n >= 1024 && (double) dead_n > 0.30 * (double) graph_n;
    if (!tail_big && !dead_big)
        return;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    av_err e;
    av_err_clear(&e);
    if (av_collection_rebuild_hnsw(c, &e) != AV_OK) {
        av_log(AV_LOG_WARN, "collection", "%s: hnsw rebuild failed: %s", c->def.name, e.msg);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double) (t1.tv_sec - t0.tv_sec) * 1e3 + (double) (t1.tv_nsec - t0.tv_nsec) / 1e6;
    av_log(AV_LOG_INFO, "collection",
           "%s: hnsw graph rebuilt over %zu live vectors in %.0f ms (was %zu, "
           "%zu dead, %zu exact tail)",
           c->def.name, live_n, ms, graph_n, dead_n, tail_n);
}

static void apply_ops(av_collection *c)
{
    av_err e;
    av_err_clear(&e);
    for (size_t i = 0; i < c->n_ops; i++) {
        const st_op *op = &c->ops[i];
        av_status st = AV_OK;
        switch (op->kind) {
        case ST_ASSIGN:
            st = av_docmap_assign(c->docmap, op->key, op->id, op->updated, op->expires, &e);
            break;
        case ST_UNMAP:
            av_docmap_unmap_key(c->docmap, op->key);
            break;
        case ST_META:
            av_docmap_set_meta(c->docmap, op->id, op->updated, op->expires);
            break;
        case ST_ATTR_ADD:
            st = av_attr_index_insert(c->ix, op->id, op->name, op->type, op->val, op->len, &e);
            break;
        case ST_ATTR_DEL:
            st = av_attr_index_remove(c->ix, op->id, op->name, op->type, op->val, op->len, &e);
            break;
        }
        if (st != AV_OK)
            (void) fatal_oom();
    }
    if (av_attr_index_flush(c->ix, &e) != AV_OK)
        (void) fatal_oom();
}

typedef struct {
    av_collection *c;
    av_err *e;
    av_status st;
    uint32_t max_docid;
    bool any;
} load_state;

static bool load_row(const av_loaded_row *r, void *p)
{
    load_state *s = p;
    av_collection *c = s->c;
    av_docid id = r->row.docid;
    if (r->vec == NULL) {
        av_log(AV_LOG_ERROR, "collection", "%s: stored row without av.vec skipped", c->def.name);
        return true;
    }
    av_arena *scratch = c->scratch;
    av_arena_reset(scratch);
    void *pg;
    av_vb_info info;
    av_err ve;
    av_err_clear(&ve);
    if (av_vb_decode(r->vec, r->vec_len, scratch, &pg, &info, &ve) != AV_OK || info.type != c->def.vtype ||
        info.dim != c->def.dimensions) {
        av_log(AV_LOG_ERROR, "collection", "%s: stored vector of docid %u is invalid, skipped", c->def.name,
               id);
        return true;
    }
    if (av_segment_insert(c->seg, id, pg, s->e) != AV_OK ||
        av_docmap_assign(c->docmap, r->key, id, r->row.updated_at, r->row.expires_at, s->e) != AV_OK) {
        s->st = AV_ERR;
        return false;
    }
    if (r->row.expires_at > 0)
        (void) av_ttl_set(c->ttl, id, r->row.expires_at);
    roaring_bitmap_add(av_live_working(c->live), id);
    ixa *l;
    size_t n;
    if (build_ixa(c, r->attrs, r->n_attrs, r->row.owner, r->row.creator, r->row.expires_at, scratch, &l, &n,
                  s->e) != AV_OK) {
        s->st = AV_ERR;
        return false;
    }
    for (size_t i = 0; i < n; i++)
        if (av_attr_index_insert(c->ix, id, l[i].name, l[i].type, l[i].val, l[i].len, s->e) != AV_OK) {
            s->st = AV_ERR;
            return false;
        }
    if (!s->any || id > s->max_docid)
        s->max_docid = id;
    s->any = true;
    return true;
}

av_collection *av_collection_create_opts(const av_coldef *def, av_store *store, const char *data_dir,
                                         const av_collection_opts *opts, av_err *e)
{
    av_collection *c = calloc(1, sizeof *c);
    if (c == NULL) {
        (void) oom(e);
        return NULL;
    }
    atomic_init(&c->refs, 1);
    atomic_init(&c->as_of, 0);
    atomic_init(&c->pub_count, 0);
    c->store = store;
    if (av_coldef_clone(def, &c->def, e) != AV_OK) {
        free(c);
        return NULL;
    }
    (void) pthread_rwlock_init(&c->lock, NULL);
    /* Writer-preferring: a steady search load must not starve the applier's graph inserts. */
    pthread_rwlockattr_t hla;
    (void) pthread_rwlockattr_init(&hla);
#ifdef __GLIBC__ /* musl (Alpine image) has no rwlock kinds */
    (void) pthread_rwlockattr_setkind_np(&hla, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    (void) pthread_rwlock_init(&c->hnsw_lock, &hla);
    (void) pthread_rwlockattr_destroy(&hla);
    (void) pthread_mutex_init(&c->wait_mu, NULL);
    pthread_condattr_t ca;
    (void) pthread_condattr_init(&ca);
    (void) pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    (void) pthread_cond_init(&c->wait_cv, &ca);
    (void) pthread_condattr_destroy(&ca);
    c->docmap = av_docmap_create();
    c->ix = av_attr_index_create();
    c->live = av_live_create();
    c->ttl = av_ttl_create();
    c->scratch = av_arena_create("coll-scratch", (size_t) 1 << 20, 0);
    c->pend = av_arena_create("coll-pend", (size_t) 1 << 16, 0);
    if (c->docmap == NULL || c->ix == NULL || c->live == NULL || c->ttl == NULL || c->scratch == NULL ||
        c->pend == NULL) {
        (void) oom(e);
        goto fail;
    }
    if (data_dir != NULL) {
        if (mkdir(data_dir, 0755) != 0 && errno != EEXIST) {
            av_err_set(e, "internal", "mkdir %s: %s", data_dir, strerror(errno));
            goto fail;
        }
        int n = snprintf(c->seg_path, sizeof c->seg_path, "%s/%s.flat", data_dir, c->def.name);
        if (n < 0 || (size_t) n >= sizeof c->seg_path) {
            av_err_set(e, "internal", "data directory path too long");
            goto fail;
        }
        c->has_seg_path = true;
    }
    if (av_segment_create_flat(c->has_seg_path ? c->seg_path : NULL, c->def.vtype, (int) c->def.dimensions,
                               opts != NULL ? opts->segment_reserve : 0, &c->seg, e) != AV_OK)
        goto fail;

    /* Rebuild from LMDB (restart path). */
    av_checkpoint_meta meta;
    if (av_checkpoint_load_meta(store, c->def.name, &meta, e) != AV_OK)
        goto fail;
    load_state ls = {c, e, AV_OK, 0, false};
    if (av_checkpoint_load_rows(store, c->def.name, load_row, &ls, e) != AV_OK || ls.st != AV_OK)
        goto fail;
    if (av_attr_index_flush(c->ix, e) != AV_OK)
        goto fail;
    av_live_publish(c->live);
    atomic_store(&c->pub_count, (size_t) roaring_bitmap_get_cardinality(av_live_current(c->live)));
    if (c->def.method == AV_INDEX_HNSW && c->def.opclass != NULL &&
        hnsw_build(c, av_live_current(c->live), &c->hnsw, &c->hnsw_tail, &c->hnsw_full, e) != AV_OK)
        goto fail;
    c->next_docid = ls.any ? ls.max_docid + 1 : 0;
    if (meta.has_next_docid && meta.next_docid > c->next_docid)
        c->next_docid = meta.next_docid;
    if (meta.has_cursor)
        atomic_store(&c->as_of, meta.cursor.block);
    av_arena_reset(c->scratch);
    return c;
fail:
    av_collection_release(c);
    return NULL;
}

av_collection *av_collection_create(const av_coldef *def, av_store *store, const char *data_dir, av_err *e)
{
    return av_collection_create_opts(def, store, data_dir, NULL, e);
}

void av_collection_retain(av_collection *c)
{
    atomic_fetch_add_explicit(&c->refs, 1, memory_order_relaxed);
}

void av_collection_release(av_collection *c)
{
    if (c == NULL)
        return;
    if (atomic_fetch_sub_explicit(&c->refs, 1, memory_order_acq_rel) != 1)
        return;
    av_segment_destroy(c->seg);
    av_segment_destroy(c->hnsw);
    roaring_bitmap_free(c->hnsw_tail);
    if (c->dropped && c->has_seg_path)
        (void) unlink(c->seg_path);
    av_docmap_destroy(c->docmap);
    av_attr_index_destroy(c->ix);
    av_live_destroy(c->live);
    av_ttl_destroy(c->ttl);
    av_arena_destroy(c->scratch);
    av_arena_destroy(c->pend);
    free(c->ops);
    av_coldef_release(&c->def);
    (void) pthread_cond_destroy(&c->wait_cv);
    (void) pthread_mutex_destroy(&c->wait_mu);
    (void) pthread_rwlock_destroy(&c->lock);
    (void) pthread_rwlock_destroy(&c->hnsw_lock);
    free(c);
}

void av_collection_destroy(av_collection *c)
{
    av_collection_release(c);
}

void av_collection_mark_dropped(av_collection *c)
{
    c->dropped = true;
}

const av_coldef *av_collection_def(const av_collection *c)
{
    return &c->def;
}

uint64_t av_collection_as_of(const av_collection *c)
{
    return atomic_load_explicit(&c->as_of, memory_order_acquire);
}

bool av_collection_is_bootstrapping(const av_collection *c)
{
    if (c == NULL)
        return false;
    return atomic_load_explicit(&c->bootstrapping, memory_order_acquire);
}

void av_collection_set_bootstrapping(av_collection *c, bool b)
{
    if (c != NULL)
        atomic_store_explicit(&c->bootstrapping, b, memory_order_release);
}

bool av_collection_has_key(const av_collection *c, const uint8_t key[32])
{
    if (c == NULL || c->docmap == NULL)
        return false;
    av_docid id = 0;
    return av_docmap_lookup(c->docmap, key, &id);
}

void av_collection_read_lock(av_collection *c)
{
    (void) pthread_rwlock_rdlock(&c->lock);
}

void av_collection_read_unlock(av_collection *c)
{
    (void) pthread_rwlock_unlock(&c->lock);
}

av_status av_collection_wait_block(av_collection *c, uint64_t min_block, int timeout_ms, av_err *e)
{
    if (av_collection_as_of(c) >= min_block)
        return AV_OK;
    struct timespec dl;
    (void) clock_gettime(CLOCK_MONOTONIC, &dl);
    int64_t ms = timeout_ms > 0 ? timeout_ms : 0;
    dl.tv_sec += (time_t) (ms / 1000);
    dl.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    (void) pthread_mutex_lock(&c->wait_mu);
    while (av_collection_as_of(c) < min_block) {
        if (pthread_cond_timedwait(&c->wait_cv, &c->wait_mu, &dl) == ETIMEDOUT)
            break;
    }
    (void) pthread_mutex_unlock(&c->wait_mu);
    uint64_t now = av_collection_as_of(c);
    if (now >= min_block)
        return AV_OK;
    av_err_set(e, "consistency_timeout", "block %llu not reached within %d ms (at block %llu)",
               (unsigned long long) min_block, timeout_ms, (unsigned long long) now);
    if (e != NULL)
        e->as_of_block = now;
    return AV_ERR;
}

/* --------------------------------------------------------------- publishing */

void av_collection_publish(av_collection *c, uint64_t block)
{
    (void) pthread_rwlock_wrlock(&c->lock);
    apply_ops(c);
    av_live_publish(c->live);
    atomic_store_explicit(&c->pub_count, (size_t) roaring_bitmap_get_cardinality(av_live_current(c->live)),
                          memory_order_release);
    atomic_store_explicit(&c->as_of, block, memory_order_release);
    (void) pthread_rwlock_unlock(&c->lock);
    stage_reset(c);
    (void) pthread_mutex_lock(&c->wait_mu);
    (void) pthread_cond_broadcast(&c->wait_cv);
    (void) pthread_mutex_unlock(&c->wait_mu);
    hnsw_maybe_rebuild(c);
}

void av_collection_discard(av_collection *c)
{
    av_live_discard(c->live);
    stage_reset(c);
}

/* -------------------------------------------------------------- reader side */

size_t av_collection_live_count(const av_collection *c)
{
    return atomic_load_explicit(&c->pub_count, memory_order_acquire);
}

const roaring_bitmap_t *av_collection_live(const av_collection *c)
{
    return av_live_current(c->live);
}

const av_docmap *av_collection_docmap(const av_collection *c)
{
    return c->docmap;
}

const av_attr_index *av_collection_attr_index(const av_collection *c)
{
    return c->ix;
}

const av_segment *av_collection_segment(const av_collection *c)
{
    return c->seg;
}

bool av_collection_has_hnsw(const av_collection *c)
{
    return c != NULL && c->hnsw != NULL;
}

av_status av_collection_rebuild_hnsw(av_collection *c, av_err *e)
{
    if (c == NULL || c->hnsw == NULL)
        return AV_OK;
    av_segment *g = NULL;
    roaring_bitmap_t *tail = NULL;
    bool full = false;
    if (hnsw_build(c, av_live_current(c->live), &g, &tail, &full, e) != AV_OK)
        return AV_ERR;
    (void) pthread_rwlock_wrlock(&c->hnsw_lock);
    av_segment *old_g = c->hnsw;
    roaring_bitmap_t *old_tail = c->hnsw_tail;
    c->hnsw = g;
    c->hnsw_tail = tail;
    c->hnsw_full = full;
    (void) pthread_rwlock_unlock(&c->hnsw_lock);
    av_segment_destroy(old_g);
    roaring_bitmap_free(old_tail);
    return AV_OK;
}

typedef struct {
    av_hit *items;
    size_t n, cap;
} hit_vec;

static bool hit_vec_push(hit_vec *v, av_docid id)
{
    if (v->n == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        av_hit *ni = realloc(v->items, ncap * sizeof *ni);
        if (ni == NULL)
            return false;
        v->items = ni;
        v->cap = ncap;
    }
    v->items[v->n].id = id;
    v->items[v->n].distance = 0.0;
    v->n++;
    return true;
}

typedef struct {
    hit_vec *v;
    bool ok;
} hit_push_ctx;

static bool hit_push_cb(uint32_t id, void *p)
{
    hit_push_ctx *pc = p;
    pc->ok = hit_vec_push(pc->v, (av_docid) id);
    return pc->ok;
}

typedef struct {
    av_hit h;
    const uint8_t *key; /* NULL sorts by docid */
} keyed_hit;

/* (distance, key) ascending, NaN last: the order of exact scans (docs/api/search.md). */
static int keyed_hit_cmp(const void *pa, const void *pb)
{
    const keyed_hit *a = pa, *b = pb;
    bool an = isnan(a->h.distance), bn = isnan(b->h.distance);
    if (an != bn)
        return an ? 1 : -1;
    if (!an && a->h.distance != b->h.distance)
        return a->h.distance < b->h.distance ? -1 : 1;
    if (a->key != NULL && b->key != NULL) {
        int r = memcmp(a->key, b->key, 32);
        if (r != 0)
            return r;
    }
    return (a->h.id > b->h.id) - (a->h.id < b->h.id);
}

av_status av_collection_search_hnsw(av_collection *c, const void *query, const av_opclass *oc,
                                    const roaring_bitmap_t *cand, int ef_search, int iterative_mode,
                                    int max_scan_tuples, size_t limit, av_hit *out, size_t *n_out, av_err *e)
{
    *n_out = 0;
    if (c->hnsw == NULL) {
        av_err_set(e, "internal", "collection %s has no hnsw index", c->def.name);
        return AV_ERR;
    }
    if (limit == 0)
        return AV_OK;
    if (oc == NULL)
        oc = c->def.opclass;
    if (cand == NULL)
        cand = av_live_current(c->live);
    if (ef_search < (int) limit)
        ef_search = limit > 1000 ? 1000 : (int) limit; /* pgvector caps hnsw.ef_search at 1000 */

    hit_vec v = {NULL, 0, 0};
    av_status st = AV_OK;
    (void) pthread_rwlock_rdlock(&c->hnsw_lock);
    av_hit *graph_hits = malloc(limit * sizeof *graph_hits);
    roaring_bitmap_t *tail = NULL;
    if (graph_hits == NULL) {
        st = oom(e);
        goto out;
    }
    size_t ng = 0;
    st = av_segment_search_hnsw(c->hnsw, query, ef_search, iterative_mode, max_scan_tuples, cand, 0, limit,
                                graph_hits, &ng, e);
    if (st != AV_OK)
        goto out;
    for (size_t i = 0; i < ng; i++)
        if (!hit_vec_push(&v, graph_hits[i].id)) {
            st = oom(e);
            goto out;
        }
    /* Vectors that are not in the graph: score them exactly. */
    if (!roaring_bitmap_is_empty(c->hnsw_tail)) {
        tail = roaring_bitmap_and(c->hnsw_tail, cand);
        if (tail == NULL) {
            st = oom(e);
            goto out;
        }
        hit_push_ctx pc = {&v, true};
        roaring_iterate(tail, hit_push_cb, &pc);
        if (!pc.ok) {
            st = oom(e);
            goto out;
        }
    }
out:
    (void) pthread_rwlock_unlock(&c->hnsw_lock);
    free(graph_hits);
    roaring_bitmap_free(tail);
    if (st != AV_OK) {
        free(v.items);
        return st;
    }

    /* Report the operator's distance on the stored raw vector, not the index distance
     * (l2 squared, negative ip, normalized cosine). */
    for (size_t i = 0; i < v.n; i++) {
        const void *vec = av_segment_get(c->seg, v.items[i].id);
        v.items[i].distance = vec != NULL ? oc->distance(query, vec) : NAN;
    }
    if (v.n == 0) {
        free(v.items);
        return AV_OK;
    }
    keyed_hit *kh = malloc(v.n * sizeof *kh);
    if (kh == NULL) {
        free(v.items);
        return oom(e);
    }
    for (size_t i = 0; i < v.n; i++) {
        kh[i].h = v.items[i];
        kh[i].key = av_docmap_key(c->docmap, v.items[i].id);
    }
    qsort(kh, v.n, sizeof *kh, keyed_hit_cmp);

    size_t take = v.n < limit ? v.n : limit;
    for (size_t i = 0; i < take; i++)
        out[i] = kh[i].h;
    free(kh);
    *n_out = take;
    free(v.items);
    return AV_OK;
}

av_status av_collection_scan_exact(const av_collection *c, const void *query, const roaring_bitmap_t *filter,
                                   size_t k, av_hit *out, size_t *n_out, av_err *e)
{
    const roaring_bitmap_t *live = av_live_current(c->live);
    roaring_bitmap_t *tmp = NULL;
    const roaring_bitmap_t *cand = live;
    if (filter != NULL) {
        tmp = roaring_bitmap_and(filter, live);
        if (tmp == NULL)
            return oom(e);
        cand = tmp;
    }
    av_status st = av_segment_scan_exact(c->seg, query, c->def.opclass, cand, k, out, n_out, e);
    roaring_bitmap_free(tmp);
    return st;
}

void av_collection_get_stats(av_collection *c, av_collection_stats *out)
{
    memset(out, 0, sizeof *out);
    av_collection_read_lock(c);
    out->live = av_collection_live_count(c);
    out->tombstones = av_docmap_high(c->docmap) > out->live ? av_docmap_high(c->docmap) - out->live : 0;
    out->segments = c->hnsw != NULL ? 2 : 1;
    av_collection_read_unlock(c);
    size_t q = 0;
    av_err e;
    av_err_clear(&e);
    if (av_store_count_quarantine(c->store, c->def.name, &q, &e) == AV_OK)
        out->quarantined = q;
}

typedef struct {
    const uint8_t *key;
    uint64_t updated, expires;
} live_ent;

typedef struct {
    const av_docmap *m;
    live_ent *v;
    size_t n, cap;
    bool oom;
} live_ctx;

static bool live_collect(uint32_t id, void *p)
{
    live_ctx *c = p;
    uint64_t u, x;
    const uint8_t *k = av_docmap_key(c->m, id);
    if (k == NULL || !av_docmap_meta(c->m, id, &u, &x))
        return true;
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        live_ent *nv = realloc(c->v, nc * sizeof *nv);
        if (nv == NULL) {
            c->oom = true;
            return false;
        }
        c->v = nv;
        c->cap = nc;
    }
    c->v[c->n++] = (live_ent){k, u, x};
    return true;
}

static int live_cmp(const void *a, const void *b)
{
    return memcmp(((const live_ent *) a)->key, ((const live_ent *) b)->key, 32);
}

void av_collection_each_live(av_collection *c, av_collection_key_cb cb, void *ctx)
{
    live_ctx lc = {c->docmap, NULL, 0, 0, false};
    av_collection_read_lock(c);
    roaring_iterate(av_live_current(c->live), live_collect, &lc);
    if (!lc.oom && lc.n > 0) {
        qsort(lc.v, lc.n, sizeof *lc.v, live_cmp);
        for (size_t i = 0; i < lc.n; i++)
            if (!cb(lc.v[i].key, lc.v[i].updated, lc.v[i].expires, ctx))
                break;
    }
    av_collection_read_unlock(c);
    free(lc.v);
}

/* ------------------------------------------------------------------ applier */

static bool is_member(const av_collection *c, const av_entity *ent)
{
    const av_attr *tag = find_attr(ent->attrs, ent->n_attrs, AV_ATTR_SCHEMA);
    size_t tl = strlen(c->def.schema_tag);
    if (tag == NULL || tag->type != AV_AT_STR || tag->len != tl ||
        (tl > 0 && memcmp(tag->val, c->def.schema_tag, tl) != 0))
        return false;
    switch (c->def.membership) {
    case AV_MEMBERSHIP_OPEN:
        return true;
    case AV_MEMBERSHIP_CREATOR:
        return av_coldef_has_writer(&c->def, ent->creator);
    case AV_MEMBERSHIP_OWNER:
        return av_coldef_has_writer(&c->def, ent->owner);
    }
    return false;
}

static void fill_row(av_row *r, av_docid id, const av_entity *ent, const uint8_t hash[32],
                     const uint8_t *blob, size_t blob_len)
{
    memset(r, 0, sizeof *r);
    r->docid = id;
    memcpy(r->owner, ent->owner, 20);
    memcpy(r->creator, ent->creator, 20);
    r->created_at = ent->created_at;
    r->updated_at = ent->updated_at;
    r->expires_at = ent->expires_at;
    memcpy(r->vec_hash, hash, 32);
    r->attrs_blob = blob;
    r->attrs_len = blob_len;
}

/* Keep the id -> key map in step: drop the old id if it changed, point the new one at key. */
static av_status sync_id(av_collection *c, av_txn *t, const av_attr *old_id, const av_attr *new_id,
                         const uint8_t key[32], av_err *e)
{
    const char *coll = c->def.name;
    if (old_id != NULL && (new_id == NULL || old_id->len != new_id->len ||
                           memcmp(old_id->val, new_id->val, old_id->len) != 0)) {
        uint8_t cur[32];
        bool found;
        if (old_id->len > 0 &&
            av_store_txn_get_id(t, coll, (const char *) old_id->val, old_id->len, cur, &found, e) != AV_OK)
            return AV_ERR;
        if (old_id->len > 0 && found && memcmp(cur, key, 32) == 0 &&
            av_store_del_id(t, coll, (const char *) old_id->val, old_id->len, e) != AV_OK)
            return AV_ERR;
    }
    if (new_id != NULL && new_id->len > 0 && new_id->len <= AV_ATTR_STR_MAX)
        return av_store_put_id(t, coll, (const char *) new_id->val, new_id->len, key, e);
    return AV_OK;
}

/* Remove a stored entity entirely (delete, lost membership, newly invalid vector). */
static av_status remove_row(av_collection *c, av_txn *t, const uint8_t key[32], const av_row *old, av_err *e)
{
    av_attr *oattrs;
    size_t on;
    if (av_attrs_unpack(old->attrs_blob, old->attrs_len, c->scratch, &oattrs, &on, e) != AV_OK)
        return AV_ERR;
    ixa *ol;
    size_t no;
    if (build_ixa(c, oattrs, on, old->owner, old->creator, old->expires_at, c->scratch, &ol, &no, e) !=
            AV_OK ||
        stage_diff(c, ol, no, old->docid, NULL, 0, 0, false, e) != AV_OK)
        return AV_ERR;
    roaring_bitmap_t *w = av_live_working(c->live);
    if (w == NULL)
        return oom(e);
    roaring_bitmap_remove(w, old->docid);
    if (c->ttl != NULL)
        av_ttl_remove(c->ttl, old->docid);
    if (av_store_del_entity(t, c->def.name, key, e) != AV_OK ||
        sync_id(c, t, find_attr(oattrs, on, AV_ATTR_ID), NULL, key, e) != AV_OK)
        return AV_ERR;
    st_op op;
    memset(&op, 0, sizeof op);
    op.kind = ST_UNMAP;
    memcpy(op.key, key, 32);
    return stage_push(c, &op, e);
}

av_status av_collection_upsert_ex(av_collection *c, av_txn *t, const av_entity *ent,
                                  av_upsert_outcome *outcome, av_err *e)
{
    const char *coll = c->def.name;
    av_arena_reset(c->scratch);
    av_row old;
    bool have_old = false;
    if (av_store_txn_get_entity(t, coll, ent->key, c->scratch, &old, &have_old, e) != AV_OK)
        return AV_ERR;

    if (!is_member(c, ent)) {
        if (!have_old) {
            *outcome = AV_UP_NOT_MEMBER;
            return AV_OK;
        }
        if (ent->updated_at < old.updated_at) {
            *outcome = AV_UP_STALE;
            return AV_OK;
        }
        *outcome = AV_UP_REMOVED;
        return remove_row(c, t, ent->key, &old, e);
    }
    uint64_t q_updated = 0;
    bool have_q = false;
    if (av_store_txn_get_quarantine(t, coll, ent->key, &q_updated, &have_q, e) != AV_OK)
        return AV_ERR;
    if ((have_old && ent->updated_at < old.updated_at) || (have_q && ent->updated_at < q_updated)) {
        *outcome = AV_UP_STALE;
        return AV_OK;
    }

    /* The vector. */
    const av_attr *vec = find_attr(ent->attrs, ent->n_attrs, AV_ATTR_VEC);
    const av_attr *new_id = find_attr(ent->attrs, ent->n_attrs, AV_ATTR_ID);
    uint8_t hash[32];
    const char *reason = NULL;
    void *pg = NULL;
    bool same_vec = false;
    av_err ve; /* function scope: reason may point into ve.vb_error */
    av_err_clear(&ve);
    if (vec == NULL || vec->type != AV_AT_BYTES) {
        reason = "VEC_MISSING";
    } else {
        sha3(vec->val, vec->len, hash, 32);
        same_vec = have_old && memcmp(hash, old.vec_hash, 32) == 0 &&
                   roaring_bitmap_contains(av_live_working(c->live), old.docid);
        if (!same_vec) {
            av_vb_info info;
            if (av_vb_decode(vec->val, vec->len, c->scratch, &pg, &info, &ve) != AV_OK)
                reason = ve.vb_error[0] != '\0' ? ve.vb_error : "VEC_INVALID";
            else if (info.type != c->def.vtype)
                reason = "VEC_TYPE_MISMATCH";
            else if (info.dim != c->def.dimensions)
                reason = "VEC_DIMENSION_MISMATCH";
        }
    }
    if (reason != NULL) {
        if (have_old && remove_row(c, t, ent->key, &old, e) != AV_OK)
            return AV_ERR;
        if (av_store_quarantine_put(t, coll, ent->key, ent->updated_at, reason, e) != AV_OK)
            return AV_ERR;
        av_log(AV_LOG_WARN, "collection", "%s: entity quarantined (%s)", coll, reason);
        *outcome = AV_UP_QUARANTINED;
        return AV_OK;
    }

    /* Stored attribute list (lite mode mirrors only vector + system fields + tag/id/doc). */
    av_attr *kept = av_arena_alloc(c->scratch, (ent->n_attrs ? ent->n_attrs : 1) * sizeof *kept, 8);
    if (kept == NULL)
        return oom(e);
    size_t nk = 0;
    for (size_t i = 0; i < ent->n_attrs; i++) {
        const char *nm = ent->attrs[i].name;
        if (c->def.mode == AV_MODE_LITE && strcmp(nm, AV_ATTR_SCHEMA) != 0 && strcmp(nm, AV_ATTR_ID) != 0 &&
            strcmp(nm, AV_ATTR_VEC) != 0 && strcmp(nm, AV_ATTR_DOC) != 0)
            continue;
        kept[nk++] = ent->attrs[i];
    }
    uint8_t *blob;
    size_t blob_len;
    if (av_attrs_pack(kept, nk, c->scratch, &blob, &blob_len, e) != AV_OK)
        return AV_ERR;

    av_attr *oattrs = NULL;
    size_t on = 0;
    ixa *ol = NULL, *nl;
    size_t no = 0, nn;
    if (have_old) {
        if (same_vec && old.attrs_len == blob_len && memcmp(old.attrs_blob, blob, blob_len) == 0 &&
            memcmp(old.owner, ent->owner, 20) == 0 && memcmp(old.creator, ent->creator, 20) == 0 &&
            old.created_at == ent->created_at && old.updated_at == ent->updated_at &&
            old.expires_at == ent->expires_at) {
            *outcome = AV_UP_UNCHANGED;
            return AV_OK;
        }
        if (av_attrs_unpack(old.attrs_blob, old.attrs_len, c->scratch, &oattrs, &on, e) != AV_OK ||
            build_ixa(c, oattrs, on, old.owner, old.creator, old.expires_at, c->scratch, &ol, &no, e) !=
                AV_OK)
            return AV_ERR;
    }
    if (build_ixa(c, kept, nk, ent->owner, ent->creator, ent->expires_at, c->scratch, &nl, &nn, e) != AV_OK)
        return AV_ERR;

    av_row row;
    st_op op;
    memset(&op, 0, sizeof op);
    if (same_vec) {
        /* Attributes only: keep the docid, update the row and postings. */
        fill_row(&row, old.docid, ent, hash, blob, blob_len);
        if (stage_diff(c, ol, no, old.docid, nl, nn, old.docid, true, e) != AV_OK ||
            av_store_put_entity(t, coll, ent->key, &row, e) != AV_OK ||
            sync_id(c, t, find_attr(oattrs, on, AV_ATTR_ID), new_id, ent->key, e) != AV_OK)
            return AV_ERR;
        op.kind = ST_META;
        op.id = old.docid;
        op.updated = ent->updated_at;
        op.expires = ent->expires_at;
        if (stage_push(c, &op, e) != AV_OK)
            return AV_ERR;
        if (c->ttl != NULL) {
            if (ent->expires_at > 0)
                (void) av_ttl_set(c->ttl, old.docid, ent->expires_at);
            else
                av_ttl_remove(c->ttl, old.docid);
        }
        *outcome = AV_UP_ATTRS_ONLY;
        return AV_OK;
    }

    /* New vector version: new docid, tombstone the old one. */
    if (c->next_docid == AV_DOCID_MAX) {
        av_err_set(e, "internal", "docid space exhausted for collection %s", coll);
        return AV_ERR;
    }
    av_docid id = c->next_docid++;
    if (av_segment_insert(c->seg, id, pg, e) != AV_OK)
        return AV_ERR;
    hnsw_add(c, id, pg); /* invisible to readers until publish adds id to the live bitmap */
    roaring_bitmap_t *w = av_live_working(c->live);
    if (w == NULL)
        return oom(e);
    if (have_old) {
        roaring_bitmap_remove(w, old.docid);
        if (c->ttl != NULL)
            av_ttl_remove(c->ttl, old.docid);
    }
    roaring_bitmap_add(w, id);
    if (c->ttl != NULL && ent->expires_at > 0)
        (void) av_ttl_set(c->ttl, id, ent->expires_at);
    fill_row(&row, id, ent, hash, blob, blob_len);
    if (stage_diff(c, ol, no, have_old ? old.docid : 0, nl, nn, id, false, e) != AV_OK ||
        av_store_put_entity(t, coll, ent->key, &row, e) != AV_OK ||
        sync_id(c, t, have_old ? find_attr(oattrs, on, AV_ATTR_ID) : NULL, new_id, ent->key, e) != AV_OK ||
        av_store_put_next_docid(t, coll, c->next_docid, e) != AV_OK)
        return AV_ERR;
    if (have_q && av_store_unquarantine(t, coll, ent->key, e) != AV_OK)
        return AV_ERR;
    op.kind = ST_ASSIGN;
    op.id = id;
    memcpy(op.key, ent->key, 32);
    op.updated = ent->updated_at;
    op.expires = ent->expires_at;
    if (stage_push(c, &op, e) != AV_OK)
        return AV_ERR;
    *outcome = have_old ? AV_UP_VECTOR_CHANGED : AV_UP_CREATED;
    return AV_OK;
}

av_status av_collection_upsert(av_collection *c, av_txn *t, const av_entity *ent, bool *changed, av_err *e)
{
    av_upsert_outcome o;
    if (av_collection_upsert_ex(c, t, ent, &o, e) != AV_OK)
        return AV_ERR;
    if (changed != NULL)
        *changed = o == AV_UP_CREATED || o == AV_UP_VECTOR_CHANGED || o == AV_UP_ATTRS_ONLY;
    return AV_OK;
}

av_status av_collection_delete(av_collection *c, av_txn *t, const uint8_t key[32], av_err *e)
{
    av_arena_reset(c->scratch);
    av_row old;
    bool have;
    if (av_store_txn_get_entity(t, c->def.name, key, c->scratch, &old, &have, e) != AV_OK ||
        av_store_unquarantine(t, c->def.name, key, e) != AV_OK)
        return AV_ERR;
    return have ? remove_row(c, t, key, &old, e) : AV_OK;
}

/* Replace one system-field posting of a stored row. */
static av_status restage_sys(av_collection *c, av_docid id, const char *name, uint8_t type,
                             const uint8_t oldw[32], const uint8_t neww[32], av_err *e)
{
    ixa o = {.type = type, .val = oldw, .len = 32}, n = {.type = type, .val = neww, .len = 32};
    (void) snprintf(o.name, sizeof o.name, "%s", name);
    (void) snprintf(n.name, sizeof n.name, "%s", name);
    if (stage_attr(c, ST_ATTR_DEL, id, &o, e) != AV_OK)
        return AV_ERR;
    return stage_attr(c, ST_ATTR_ADD, id, &n, e);
}

av_status av_collection_extend(av_collection *c, av_txn *t, const uint8_t key[32], uint64_t expires_at,
                               av_err *e)
{
    av_arena_reset(c->scratch);
    av_row row;
    bool have;
    if (av_store_txn_get_entity(t, c->def.name, key, c->scratch, &row, &have, e) != AV_OK)
        return AV_ERR;
    if (!have)
        return AV_OK;
    uint8_t ow[32], nw[32];
    u64_word(ow, row.expires_at);
    u64_word(nw, expires_at);
    uint64_t old_expires = row.expires_at;
    row.expires_at = expires_at;
    if (av_store_put_entity(t, c->def.name, key, &row, e) != AV_OK)
        return AV_ERR;
    if (old_expires != expires_at && restage_sys(c, row.docid, "$expiresAt", AV_AT_U64, ow, nw, e) != AV_OK)
        return AV_ERR;
    st_op op;
    memset(&op, 0, sizeof op);
    op.kind = ST_META;
    op.id = row.docid;
    op.updated = row.updated_at;
    op.expires = expires_at;
    if (c->ttl != NULL) {
        if (expires_at > 0)
            (void) av_ttl_set(c->ttl, row.docid, expires_at);
        else
            av_ttl_remove(c->ttl, row.docid);
    }
    return stage_push(c, &op, e);
}

av_status av_collection_transfer(av_collection *c, av_txn *t, const uint8_t key[32],
                                 const uint8_t new_owner[20], av_err *e)
{
    av_arena_reset(c->scratch);
    av_row row;
    bool have;
    if (av_store_txn_get_entity(t, c->def.name, key, c->scratch, &row, &have, e) != AV_OK)
        return AV_ERR;
    if (!have)
        return AV_OK;
    if (c->def.membership == AV_MEMBERSHIP_OWNER && !av_coldef_has_writer(&c->def, new_owner))
        return remove_row(c, t, key, &row, e);
    uint8_t ow[32], nw[32];
    addr_word(ow, row.owner);
    addr_word(nw, new_owner);
    bool same = memcmp(row.owner, new_owner, 20) == 0;
    memcpy(row.owner, new_owner, 20);
    if (av_store_put_entity(t, c->def.name, key, &row, e) != AV_OK)
        return AV_ERR;
    return same ? AV_OK : restage_sys(c, row.docid, "$owner", AV_AT_ADDR, ow, nw, e);
}

av_status av_collection_expire(av_collection *c, av_txn *t, uint64_t block, size_t budget, av_err *e)
{
    if (c == NULL || c->ttl == NULL)
        return AV_OK;
    if (budget == 0)
        budget = 10000;
    uint32_t docids[256];
    size_t total = 0;
    while (total < budget) {
        size_t batch = budget - total < 256 ? budget - total : 256;
        size_t popped = av_ttl_pop_expired(c->ttl, block, docids, batch);
        if (popped == 0)
            break;
        for (size_t i = 0; i < popped; i++) {
            const uint8_t *k = av_docmap_key(c->docmap, docids[i]);
            if (k != NULL) {
                if (av_collection_delete(c, t, k, e) != AV_OK)
                    return AV_ERR;
            }
        }
        total += popped;
        if (popped < batch)
            break;
    }
    return AV_OK;
}
