/*
 * index/segment.c: flat and HNSW segments. See segment.h.
 */
#include "index/segment.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "fmgr.h"
#include "hnsw.h"
#include "pgvector/hnswscan_mem.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "store/arena_file.h"
#include "varatt.h"
#include "version.h"

#define DEFAULT_MAX_DOCIDS ((size_t) 1 << 28)

struct av_segment {
    av_seg_kind kind;
    av_vtype type;
    int dim;
    const av_opclass *oc;
    av_arena_file *data;
    av_arena_file *table; /* uint64_t offsets indexed by docid, 0 = absent */
    _Atomic size_t table_len;
    _Atomic size_t count;
    uint32_t min_docid;
    uint32_t max_docid;
    bool has_docid_range;
    bool sealed;

    /* HNSW specific */
    int m;
    int ef_construction;
    HnswGraph *graph;
    HnswMemBuildState bs;
    FmgrInfo procinfo;
    FmgrInfo normprocinfo;
    const HnswTypeInfo *typeInfo;
    HnswSupport support;
    uint64_t prng_seed;
};

av_status av_segment_create_flat(const char *path, av_vtype type, int dim, size_t reserve_bytes, av_segment **out,
                                 av_err *e)
{
    av_segment *s = calloc(1, sizeof *s);
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->kind = AV_SEG_FLAT;
    s->type = type;
    s->dim = dim;
    if (av_arena_file_open(path, reserve_bytes ? reserve_bytes : AV_SEG_DEFAULT_RESERVE, AV_AF_TRUNCATE, &s->data, e) !=
            AV_OK ||
        av_arena_file_open(NULL, DEFAULT_MAX_DOCIDS * sizeof(uint64_t), 0, &s->table, e) != AV_OK) {
        av_segment_destroy(s);
        return AV_ERR;
    }
    /* Offset 0 means "absent": burn the first 8 bytes. */
    if (av_arena_file_alloc(s->data, 8, 8, e) == NULL) {
        av_segment_destroy(s);
        return AV_ERR;
    }
    *out = s;
    return AV_OK;
}

static void *segment_hnsw_alloc(Size size, void *state)
{
    av_segment *s = (av_segment *) state;
    HnswMemBuildState *buildstate = &s->bs;
    Size alignedSize = MAXALIGN(size);
    Size newMemoryUsed = add_size(buildstate->graph->memoryUsed, alignedSize);
    if (newMemoryUsed > buildstate->graph->memoryTotal)
        elog(ERROR, "hnsw allocator out of memory");

    av_err err;
    void *chunk = av_arena_file_alloc(s->data, alignedSize, 8, &err);
    if (chunk == NULL)
        elog(ERROR, "hnsw arena file alloc failed");

    buildstate->graph->memoryUsed = (Size) av_arena_file_used(s->data);
    return chunk;
}

static size_t estimate_hnsw_reserve(size_t max_vectors, av_vtype type, int dim, int m)
{
    (void) type;
    size_t val_sz = (size_t) dim * sizeof(float) + 4;
    size_t per_vec = val_sz + sizeof(HnswElementData) + (size_t) (3 * m * 16) + 64;
    size_t total = sizeof(HnswGraph) + (size_t) ((double) (max_vectors * per_vec) * 1.2) + (1024 * 1024);
    return total;
}

av_status av_segment_create_hnsw(const char *path, av_vtype type, int dim, const av_opclass *oc,
                                 int m, int ef_construction, size_t reserve_bytes,
                                 av_segment **out, av_err *e)
{
    av_segment *s = calloc(1, sizeof *s);
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->kind = AV_SEG_HNSW;
    s->type = type;
    s->dim = dim;
    s->oc = oc;
    s->m = m > 0 ? m : HNSW_DEFAULT_M;
    s->ef_construction = ef_construction > 0 ? ef_construction : HNSW_DEFAULT_EF_CONSTRUCTION;
    s->prng_seed = 0x41524B4956ULL;

    size_t actual_reserve = reserve_bytes;
    if (actual_reserve == 0) {
        actual_reserve = estimate_hnsw_reserve(64000, type, dim, s->m);
    }

    if (av_arena_file_open(path, actual_reserve, AV_AF_TRUNCATE, &s->data, e) != AV_OK ||
        av_arena_file_open(NULL, DEFAULT_MAX_DOCIDS * sizeof(uint64_t), 0, &s->table, e) != AV_OK) {
        av_segment_destroy(s);
        return AV_ERR;
    }

    /* Allocate and initialize HnswGraph at the base of data */
    void *gmem = av_arena_file_alloc(s->data, sizeof(HnswGraph), 8, e);
    if (gmem == NULL) {
        av_segment_destroy(s);
        return AV_ERR;
    }
    s->graph = (HnswGraph *) gmem;
    AvHnswInitGraph(s->graph, (char *) s->graph, actual_reserve);
    s->graph->memoryUsed = MAXALIGN(sizeof(HnswGraph));

    /* Setup support and typeInfo */
    s->typeInfo = AvHnswGetTypeInfo((int) type);
    av_pgfn index_fn = oc != NULL ? av_opclass_index_pgfn(oc) : NULL;
    if (index_fn != NULL) {
        av_fmgr_info(&s->procinfo, index_fn);
        s->support.procinfo = &s->procinfo;
    }
    if (oc != NULL && oc->normalize) {
        av_pgfn norm_fn = av_opclass_norm_pgfn(oc);
        if (norm_fn != NULL) {
            av_fmgr_info(&s->normprocinfo, norm_fn);
            s->support.normprocinfo = &s->normprocinfo;
        }
    }
    s->support.collation = InvalidOid;

    /* Setup build state */
    s->bs.typeInfo = s->typeInfo;
    s->bs.dimensions = dim;
    s->bs.m = s->m;
    s->bs.efConstruction = s->ef_construction;
    s->bs.support = s->support;
    s->bs.graph = s->graph;
    s->bs.ml = HnswGetMl(s->bs.m);
    s->bs.maxLevel = (int) HnswGetMaxLevel((size_t) s->bs.m);
    s->bs.hnswarea = (char *) s->graph;
    AvHnswInitAllocator(&s->bs.allocator, segment_hnsw_alloc, s);

    *out = s;
    return AV_OK;
}

av_status av_segment_seal(av_segment *s, av_err *e)
{
    if (s == NULL)
        return AV_OK;
    if (s->kind != AV_SEG_HNSW || s->sealed)
        return AV_OK;

    av_seg_footer footer;
    memset(&footer, 0, sizeof(footer));
    memcpy(footer.magic, AV_SEG_MAGIC, 6);
    footer.magic[6] = '\0';
    footer.magic[7] = '\0';
    strncpy(footer.pgvector_tag, AV_PGVECTOR_TAG, sizeof(footer.pgvector_tag) - 1);
    if (s->oc != NULL && s->oc->name != NULL)
        strncpy(footer.opclass_name, s->oc->name, sizeof(footer.opclass_name) - 1);
    footer.m = (uint32_t) s->m;
    footer.ef_construction = (uint32_t) s->ef_construction;
    footer.dimensions = (uint32_t) s->dim;
    footer.vtype = (uint32_t) s->type;
    footer.entry_point_offset = (uint64_t) HnswPtrOffset(s->graph->entryPoint);
    footer.count = (uint64_t) atomic_load_explicit(&s->count, memory_order_acquire);
    footer.min_docid = s->min_docid;
    footer.max_docid = s->max_docid;
    footer.prng_seed = s->prng_seed;
    footer.arena_bytes = (uint64_t) s->graph->memoryUsed;

    void *dst = av_arena_file_alloc(s->data, sizeof(footer), 8, e);
    if (dst == NULL)
        return AV_ERR;
    memcpy(dst, &footer, sizeof(footer));

    if (av_arena_file_sync(s->data, e) != AV_OK)
        return AV_ERR;
    s->sealed = true;
    return AV_OK;
}

bool av_segment_is_sealed(const av_segment *s)
{
    return s != NULL && s->sealed;
}

av_status av_segment_open_sealed(const char *path, av_segment **out, av_err *e)
{
    *out = NULL;
    if (path == NULL) {
        av_err_set(e, "invalid_argument", "path required for sealed segment");
        return AV_ERR;
    }

    av_segment *s = calloc(1, sizeof *s);
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }

    if (av_arena_file_open(path, 0, AV_AF_READONLY, &s->data, e) != AV_OK ||
        av_arena_file_open(NULL, DEFAULT_MAX_DOCIDS * sizeof(uint64_t), 0, &s->table, e) != AV_OK) {
        av_segment_destroy(s);
        return AV_ERR;
    }

    size_t sz = av_arena_file_used(s->data);
    if (sz < sizeof(HnswGraph) + sizeof(av_seg_footer)) {
        av_segment_destroy(s);
        av_err_set(e, "invalid_argument", "segment file too small or corrupted");
        return AV_ERR;
    }

    char *base = (char *) av_arena_file_base(s->data);
    const av_seg_footer *footer = (const av_seg_footer *) (base + sz - sizeof(av_seg_footer));

    if (memcmp(footer->magic, AV_SEG_MAGIC, 6) != 0) {
        av_segment_destroy(s);
        av_err_set(e, "invalid_argument", "invalid segment footer magic");
        return AV_ERR;
    }

    s->kind = AV_SEG_HNSW;
    s->sealed = true;
    s->type = (av_vtype) footer->vtype;
    s->dim = (int) footer->dimensions;
    s->m = (int) footer->m;
    s->ef_construction = (int) footer->ef_construction;
    s->min_docid = footer->min_docid;
    s->max_docid = footer->max_docid;
    s->has_docid_range = true;
    s->prng_seed = footer->prng_seed;
    atomic_store_explicit(&s->count, (size_t) footer->count, memory_order_relaxed);

    s->oc = av_opclass_find(footer->opclass_name);
    s->graph = (HnswGraph *) base;

    /* Setup typeInfo & support */
    s->typeInfo = AvHnswGetTypeInfo((int) s->type);
    if (s->oc != NULL) {
        av_pgfn index_fn = av_opclass_index_pgfn(s->oc);
        if (index_fn != NULL) {
            av_fmgr_info(&s->procinfo, index_fn);
            s->support.procinfo = &s->procinfo;
        }
        if (s->oc->normalize) {
            av_pgfn norm_fn = av_opclass_norm_pgfn(s->oc);
            if (norm_fn != NULL) {
                av_fmgr_info(&s->normprocinfo, norm_fn);
                s->support.normprocinfo = &s->normprocinfo;
            }
        }
    }
    s->support.collation = InvalidOid;

    /* Populate docid -> offset table from elements */
    if (footer->count > 0 && footer->max_docid < DEFAULT_MAX_DOCIDS) {
        size_t need = ((size_t) footer->max_docid + 1) * sizeof(uint64_t);
        if (av_arena_file_ensure(s->table, need, e) != AV_OK) {
            av_segment_destroy(s);
            return AV_ERR;
        }
        uint64_t *tbl = av_arena_file_base(s->table);
        HnswElement curr = HnswPtrAccess(base, s->graph->head);
        while (curr != NULL) {
            for (int i = 0; i < (int) curr->heaptidsLength; i++) {
                av_docid docid = av_tid_to_docid(&curr->heaptids[i]);
                if (docid <= footer->max_docid) {
                    char *valPtr = HnswPtrAccess(base, curr->value);
                    if (valPtr != NULL)
                        tbl[docid] = (uint64_t) (valPtr - base);
                }
            }
            curr = HnswPtrAccess(base, curr->next);
        }
        atomic_store_explicit(&s->table_len, (size_t) footer->max_docid + 1, memory_order_release);
    }

    *out = s;
    return AV_OK;
}

void av_segment_destroy(av_segment *s)
{
    if (s == NULL)
        return;
    av_arena_file_close(s->data);
    av_arena_file_close(s->table);
    free(s);
}

av_seg_kind av_segment_kind(const av_segment *s)
{
    return s->kind;
}

static av_status av_hnsw_insert(av_segment *s, av_docid id, const void *pgvec, av_err *e)
{
    if (s->sealed) {
        av_err_set(e, "internal", "cannot insert into sealed segment");
        return AV_ERR;
    }
    size_t need = ((size_t) id + 1) * sizeof(uint64_t);
    if (need > av_arena_file_reserved(s->table)) {
        av_err_set(e, "internal", "docid %u exceeds the segment table", id);
        return AV_ERR;
    }
    if (av_pgvec_dim(s->type, pgvec) != s->dim) {
        av_err_set(e, "dimension_mismatch", "vector dimension differs from the segment (docid %u)", id);
        return AV_ERR;
    }
    if (av_arena_file_ensure(s->table, need, e) != AV_OK)
        return AV_ERR;

    uint64_t *table = av_arena_file_base(s->table);
    if (table[id] != 0) {
        av_err_set(e, "internal", "docid %u is already in the segment", id);
        return AV_ERR;
    }

    HnswGraph *graph = s->graph;
    HnswMemBuildState *bs = &s->bs;
    char *base = (char *) av_arena_file_base(s->data);
    Datum value = PointerGetDatum(pgvec);

    if (bs->support.normprocinfo != NULL) {
        value = HnswNormValue(bs->typeInfo, bs->support.collation, value);
    }

    ItemPointerData tid;
    av_docid_to_tid(id, &tid);
    HnswCheckDim(bs->dimensions, bs->typeInfo, bs->support.collation, value);
    Size valueSize = (Size) VARSIZE_ANY(DatumGetPointer(value));
    Size memoryMargin = (graph->memoryTotal > 4 * 1024 * 1024) ? (1024 * 1024) : (graph->memoryTotal / 16);

    LWLockAcquire(&graph->allocatorLock, LW_EXCLUSIVE);
    if (add_size(graph->memoryUsed, memoryMargin) >= graph->memoryTotal) {
        LWLockRelease(&graph->allocatorLock);
        return AV_SEAL_NEEDED;
    }
    HnswElement element = HnswInitElement(base, &tid, bs->m, bs->ml, bs->maxLevel, &bs->allocator);
    Pointer valuePtr = HnswAlloc(&bs->allocator, valueSize);
    LWLockRelease(&graph->allocatorLock);

    memcpy(valuePtr, DatumGetPointer(value), valueSize);
    HnswPtrStore(base, element->value, (char *) valuePtr);
    LWLockInitialize(&element->lock, hnsw_lock_tranche_id);

    AvHnswInsertTupleInMemory(bs, element);

    table[id] = (uint64_t) ((char *) valuePtr - base);
    size_t len = atomic_load_explicit(&s->table_len, memory_order_relaxed);
    if ((size_t) id + 1 > len)
        atomic_store_explicit(&s->table_len, (size_t) id + 1, memory_order_release);
    atomic_fetch_add_explicit(&s->count, 1, memory_order_release);

    if (!s->has_docid_range) {
        s->min_docid = id;
        s->max_docid = id;
        s->has_docid_range = true;
    } else {
        if (id < s->min_docid) s->min_docid = id;
        if (id > s->max_docid) s->max_docid = id;
    }

    return AV_OK;
}

av_status av_segment_insert(av_segment *s, av_docid id, const void *pgvec, av_err *e)
{
    if (s->kind == AV_SEG_HNSW)
        return av_hnsw_insert(s, id, pgvec, e);

    size_t need = ((size_t) id + 1) * sizeof(uint64_t);
    if (need > av_arena_file_reserved(s->table)) {
        av_err_set(e, "internal", "docid %u exceeds the segment table", id);
        return AV_ERR;
    }
    if (av_pgvec_dim(s->type, pgvec) != s->dim) {
        av_err_set(e, "dimension_mismatch", "vector dimension differs from the segment (docid %u)", id);
        return AV_ERR;
    }
    if (av_arena_file_ensure(s->table, need, e) != AV_OK)
        return AV_ERR;
    uint64_t *table = av_arena_file_base(s->table);
    if (table[id] != 0) {
        av_err_set(e, "internal", "docid %u is already in the segment", id);
        return AV_ERR;
    }
    size_t n = VARSIZE(pgvec);
    void *dst = av_arena_file_alloc(s->data, n, 8, e);
    if (dst == NULL)
        return AV_ERR;
    memcpy(dst, pgvec, n);
    table[id] = (uint64_t) ((uint8_t *) dst - (uint8_t *) av_arena_file_base(s->data));
    size_t len = atomic_load_explicit(&s->table_len, memory_order_relaxed);
    if ((size_t) id + 1 > len)
        atomic_store_explicit(&s->table_len, (size_t) id + 1, memory_order_release);
    atomic_fetch_add_explicit(&s->count, 1, memory_order_release);
    return AV_OK;
}

size_t av_segment_count(const av_segment *s)
{
    return atomic_load_explicit(&s->count, memory_order_acquire);
}

const void *av_segment_get(const av_segment *s, av_docid id)
{
    if ((size_t) id >= atomic_load_explicit(&s->table_len, memory_order_acquire))
        return NULL;
    uint64_t off = ((const uint64_t *) av_arena_file_base(s->table))[id];
    return off != 0 ? (const uint8_t *) av_arena_file_base(s->data) + off : NULL;
}

av_status av_segment_search_hnsw(const av_segment *s, const void *query, int ef_search,
                                 int iterative_mode, int max_scan_tuples,
                                 const roaring_bitmap_t *cand, size_t offset, size_t k,
                                 av_hit *out, size_t *n_out, av_err *e)
{
    *n_out = 0;
    if (s->kind != AV_SEG_HNSW) {
        av_err_set(e, "internal", "segment is not HNSW");
        return AV_ERR;
    }
    if (av_pgvec_dim(s->type, query) != s->dim) {
        av_err_set(e, "dimension_mismatch", "query dimension %d differs from column dimension %d",
                   av_pgvec_dim(s->type, query), s->dim);
        return AV_ERR;
    }
    if (k == 0)
        return AV_OK;

    char *base = (char *) av_arena_file_base(s->data);
    Datum qval = PointerGetDatum(query);

    if (s->support.normprocinfo != NULL) {
        qval = HnswNormValue(s->typeInfo, s->support.collation, qval);
    }

    bool prev_ro = av_lwlock_readonly_arena;
    av_lwlock_readonly_arena = s->sealed;

    HnswSupport supp = s->support;
    av_status st = av_hnsw_scan_mem(base, s->graph, &supp, s->m, qval, ef_search,
                                   iterative_mode, max_scan_tuples, cand,
                                   offset, k, out, n_out, e);

    av_lwlock_readonly_arena = prev_ro;
    return st;
}

/* ---- bounded max-heap of the k best (smallest) hits; "worse" = larger (distance, id), NaN largest ---- */

static bool hit_worse(const av_hit *a, const av_hit *b)
{
    bool an = isnan(a->distance), bn = isnan(b->distance);
    if (an != bn)
        return an;
    if (!an && a->distance != b->distance)
        return a->distance > b->distance;
    return a->id > b->id;
}

static void sift_down(av_hit *h, size_t n, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, w = i;
        if (l < n && hit_worse(&h[l], &h[w]))
            w = l;
        if (r < n && hit_worse(&h[r], &h[w]))
            w = r;
        if (w == i)
            return;
        av_hit t = h[i];
        h[i] = h[w];
        h[w] = t;
        i = w;
    }
}

static void sift_up(av_hit *h, size_t i)
{
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!hit_worse(&h[i], &h[p]))
            return;
        av_hit t = h[i];
        h[i] = h[p];
        h[p] = t;
        i = p;
    }
}

typedef struct {
    const av_segment *s;
    const void *query;
    const av_opclass *oc;
    av_hit *heap;
    size_t k, n;
} scan_ctx;

static void scan_one(scan_ctx *c, av_docid id)
{
    const void *v = av_segment_get(c->s, id);
    if (v == NULL)
        return;
    av_hit h = {id, c->oc->distance(c->query, v)};
    if (c->n < c->k) {
        c->heap[c->n] = h;
        sift_up(c->heap, c->n++);
    } else if (hit_worse(&c->heap[0], &h)) {
        c->heap[0] = h;
        sift_down(c->heap, c->n, 0);
    }
}

static bool scan_cb(uint32_t id, void *p)
{
    scan_one(p, id);
    return true;
}

av_status av_segment_scan_exact(const av_segment *s, const void *query, const av_opclass *oc,
                                const roaring_bitmap_t *cand, size_t k, av_hit *out, size_t *n_out, av_err *e)
{
    *n_out = 0;
    if (oc->type != s->type) {
        av_err_set(e, "invalid_argument", "opclass %s does not match the segment column type", oc->name);
        return AV_ERR;
    }
    if (av_pgvec_dim(s->type, query) != s->dim) {
        av_err_set(e, "dimension_mismatch", "query dimension %d differs from column dimension %d",
                   av_pgvec_dim(s->type, query), s->dim);
        return AV_ERR;
    }
    if (k == 0)
        return AV_OK;
    scan_ctx c = {s, query, oc, out, k, 0};
    if (cand != NULL) {
        roaring_iterate(cand, scan_cb, &c);
    } else {
        size_t len = atomic_load_explicit(&s->table_len, memory_order_acquire);
        for (size_t i = 0; i < len; i++)
            scan_one(&c, (av_docid) i);
    }
    /* Heap-sort in place: repeatedly move the worst to the end -> ascending order. */
    size_t n = c.n;
    for (size_t m = n; m > 1; m--) {
        av_hit t = out[0];
        out[0] = out[m - 1];
        out[m - 1] = t;
        sift_down(out, m - 1, 0);
    }
    *n_out = n;
    return AV_OK;
}
