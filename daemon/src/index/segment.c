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
#include "utils/elog.h"
#include "utils/memutils.h"
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

av_status av_segment_create_flat(const char *path, av_vtype type, int dim, size_t reserve_bytes,
                                 av_segment **out, av_err *e)
{
    av_segment *s = calloc(1, sizeof *s);
    if (s == NULL) {
        av_err_set(e, "internal", "out of memory");
        return AV_ERR;
    }
    s->kind = AV_SEG_FLAT;
    s->type = type;
    s->dim = dim;
    if (av_arena_file_open(path, reserve_bytes ? reserve_bytes : AV_SEG_DEFAULT_RESERVE, AV_AF_TRUNCATE,
                           &s->data, e) != AV_OK ||
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
    size_t d = dim > 0 ? (size_t) dim : 1;
    size_t val_sz;
    switch (type) {
    case AV_VT_HALFVEC:
        val_sz = 2 * d + 8;
        break;
    case AV_VT_BIT:
        val_sz = (d + 7) / 8 + 8;
        break;
    case AV_VT_SPARSEVEC:
        val_sz = 8 * (d < 1000 ? d : 1000) + 16; /* nnz unknown: assume at most 1000 */
        break;
    default:
        val_sz = 4 * d + 8;
        break;
    }
    /* element + value + level-0 (2m) and upper-level (m per level, ~1/ln(m) levels) neighbour arrays */
    size_t per_vec = val_sz + sizeof(HnswElementData) + (size_t) (3 * m * 16) + 64;
    size_t total = sizeof(HnswGraph) + (size_t) ((double) (max_vectors * per_vec) * 1.2) + (1024 * 1024);
    return total;
}

size_t av_segment_hnsw_reserve(av_vtype type, int dim, int m, size_t max_vectors)
{
    size_t r = estimate_hnsw_reserve(max_vectors, type, dim, m > 0 ? m : HNSW_DEFAULT_M);
    return r < AV_SEG_DEFAULT_RESERVE ? r : AV_SEG_DEFAULT_RESERVE;
}

av_status av_segment_create_hnsw(const char *path, av_vtype type, int dim, const av_opclass *oc, int m,
                                 int ef_construction, size_t reserve_bytes, av_segment **out, av_err *e)
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
    if (base == NULL) {
        av_segment_destroy(s);
        av_err_set(e, "internal", "segment file is not mapped");
        return AV_ERR;
    }
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

    /* pgvector code reports errors with ereport(ERROR): catch them here, never abort the daemon. */
    MemoryContext old = CurrentMemoryContext;
    MemoryContext volatile ctx = NULL;
    volatile av_status st = AV_OK;
    char *volatile valuePtr = NULL;
    PG_TRY();
    {
        ctx = AllocSetContextCreate(NULL, "hnsw_insert", ALLOCSET_DEFAULT_SIZES);
        MemoryContextSwitchTo(ctx);
        Datum in = PointerGetDatum(pgvec);
        bool isnull = false;
        Datum value;
        /* Upstream InsertTuple: check the value, skip zero-norm vectors, normalize (cosine). */
        if (!HnswFormIndexValue(&value, &in, &isnull, bs->typeInfo, &bs->support)) {
            st = AV_SKIPPED;
        } else {
            ItemPointerData tid;
            av_docid_to_tid(id, &tid);
            HnswCheckDim(bs->dimensions, bs->typeInfo, bs->support.collation, value);
            Size valueSize = (Size) VARSIZE_ANY(DatumGetPointer(value));
            Size memoryMargin =
                (graph->memoryTotal > 4 * 1024 * 1024) ? (1024 * 1024) : (graph->memoryTotal / 16);

            LWLockAcquire(&graph->allocatorLock, LW_EXCLUSIVE);
            if (add_size(add_size(graph->memoryUsed, memoryMargin), valueSize) >= graph->memoryTotal) {
                LWLockRelease(&graph->allocatorLock);
                st = AV_SEAL_NEEDED;
            } else {
                HnswElement element =
                    HnswInitElement(base, &tid, bs->m, bs->ml, bs->maxLevel, &bs->allocator);
                valuePtr = HnswAlloc(&bs->allocator, valueSize);
                LWLockRelease(&graph->allocatorLock);

                memcpy(valuePtr, DatumGetPointer(value), valueSize);
                HnswPtrStore(base, element->value, (char *) valuePtr);
                LWLockInitialize(&element->lock, hnsw_lock_tranche_id);

                AvHnswInsertTupleInMemory(bs, element);
            }
        }
    }
    PG_CATCH();
    {
        av_err_set(e, "internal", "hnsw insert of docid %u: %s", id, av_pg_error_data()->message);
        FlushErrorState();
        st = AV_ERR;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    if (ctx != NULL)
        MemoryContextDelete(ctx);
    if (st != AV_OK)
        return st;
    if (valuePtr == NULL) {
        av_err_set(e, "internal", "hnsw insert of docid %u stored no value", id);
        return AV_ERR;
    }

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
        if (id < s->min_docid)
            s->min_docid = id;
        if (id > s->max_docid)
            s->max_docid = id;
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

av_status av_segment_search_hnsw(const av_segment *s, const void *query, int ef_search, int iterative_mode,
                                 int max_scan_tuples, const roaring_bitmap_t *cand, size_t offset, size_t k,
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
    bool prev_ro = av_lwlock_readonly_arena;
    av_lwlock_readonly_arena = s->sealed;

    MemoryContext old = CurrentMemoryContext;
    MemoryContext volatile ctx = NULL;
    volatile av_status st = AV_OK;
    PG_TRY();
    {
        ctx = AllocSetContextCreate(NULL, "hnsw_search", ALLOCSET_DEFAULT_SIZES);
        MemoryContextSwitchTo(ctx);
        Datum qval = PointerGetDatum(query);
        HnswSupport supp = s->support;
        /* Upstream hnswscan.c: a zero-norm query has no cosine neighbours (no index results). */
        if (supp.normprocinfo != NULL && !HnswCheckNorm(&supp, qval)) {
            *n_out = 0;
        } else {
            if (supp.normprocinfo != NULL)
                qval = HnswNormValue(s->typeInfo, supp.collation, qval);
            st = av_hnsw_scan_mem(base, s->graph, &supp, s->m, qval, ef_search, iterative_mode,
                                  max_scan_tuples, cand, offset, k, out, n_out, e);
        }
    }
    PG_CATCH();
    {
        av_err_set(e, "internal", "hnsw search: %s", av_pg_error_data()->message);
        FlushErrorState();
        *n_out = 0;
        st = AV_ERR;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    if (ctx != NULL)
        MemoryContextDelete(ctx);

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

/* ------------------------------------------------------------------ HNSW persistence */

#include "bitvec.h"
#include "halfvec.h"
#include "sparsevec.h"
#include "storage/spin.h"
#include "utils/varbit.h"
#include "vector.h"

#define GRAPH_HDR ((size_t) MAXALIGN(sizeof(HnswGraph)))

av_status av_segment_hnsw_snapshot(const av_segment *s, void **bytes, size_t *len, av_seg_hnsw_info *info,
                                   av_err *e)
{
    *bytes = NULL;
    *len = 0;
    if (s == NULL || s->kind != AV_SEG_HNSW || s->graph == NULL) {
        av_err_set(e, "internal", "not an HNSW segment");
        return AV_ERR;
    }
    size_t used = av_arena_file_used(s->data);
    void *copy = malloc(used > 0 ? used : 1);
    if (copy == NULL) {
        av_err_set(e, "internal", "out of memory copying the HNSW graph (%zu bytes)", used);
        return AV_ERR;
    }
    memcpy(copy, av_arena_file_base(s->data), used);
    memset(info, 0, sizeof(*info));
    info->type = s->type;
    info->dim = s->dim;
    info->m = s->m;
    info->ef_construction = s->ef_construction;
    info->count = atomic_load_explicit(&s->count, memory_order_acquire);
    info->arena_bytes = used;
    info->min_docid = s->min_docid;
    info->max_docid = s->max_docid;
    *bytes = copy;
    *len = used;
    return AV_OK;
}

/* Offset of a relptr target in the image, or SIZE_MAX for NULL. False when out of bounds,
 * misaligned or overlapping the graph header, or when fewer than need bytes follow it. */
static bool img_ptr(Size raw, size_t len, size_t need, size_t *off)
{
    if (raw == 0) {
        *off = SIZE_MAX;
        return true;
    }
    size_t o = (size_t) raw - 1;
    if (o < GRAPH_HDR || o % MAXIMUM_ALIGNOF != 0 || o > len || need > len - o)
        return false;
    *off = o;
    return true;
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *) a, y = *(const size_t *) b;
    return x < y ? -1 : x > y;
}

typedef struct {
    size_t lo, hi;
} img_region;

static int cmp_region(const void *a, const void *b)
{
    const img_region *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* Record [off, off + n): every structure of the image must be disjoint, because restore resets the
 * lock words and readers write to them, which would corrupt any structure overlapping them. */
static bool img_region_add(img_region **r, size_t *n, size_t *cap, size_t off, size_t len)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        img_region *nr = realloc(*r, nc * sizeof *nr);
        if (nr == NULL)
            return false;
        *r = nr;
        *cap = nc;
    }
    (*r)[*n].lo = off;
    (*r)[*n].hi = off + len;
    (*n)++;
    return true;
}

static bool img_is_element(const size_t *offs, size_t n, size_t off)
{
    return bsearch(&off, offs, n, sizeof(size_t), cmp_size) != NULL;
}

/* A stored index value must have the exact pgvector layout for (type, dim). */
static bool img_value_ok(const char *b, size_t len, size_t off, av_vtype type, int dim)
{
    if (off == SIZE_MAX || len - off < 8)
        return false;
    const char *p = b + off;
    if (!VARATT_IS_4B_U(p))
        return false;
    size_t sz = VARSIZE(p);
    if (sz > len - off)
        return false;
    switch (type) {
    case AV_VT_VECTOR: {
        const Vector *v = (const Vector *) p;
        return v->dim == dim && v->unused == 0 && sz == VECTOR_SIZE((size_t) dim);
    }
    case AV_VT_HALFVEC: {
        const HalfVector *v = (const HalfVector *) p;
        return v->dim == dim && v->unused == 0 && sz == HALFVEC_SIZE((size_t) dim);
    }
    case AV_VT_SPARSEVEC: {
        if (sz < offsetof(SparseVector, indices))
            return false;
        const SparseVector *v = (const SparseVector *) p;
        if (v->dim != dim || v->nnz < 0 || v->nnz > dim || v->unused != 0 || sz != SPARSEVEC_SIZE(v->nnz))
            return false;
        for (int i = 0; i < v->nnz; i++)
            if (v->indices[i] < 0 || v->indices[i] >= dim || (i > 0 && v->indices[i] <= v->indices[i - 1]))
                return false;
        return true;
    }
    case AV_VT_BIT: {
        if (sz < VARHDRSZ + sizeof(int32))
            return false;
        const VarBit *v = (const VarBit *) p;
        return v->bit_len == dim && sz == (size_t) VARBITTOTALLEN((size_t) dim);
    }
    }
    return false;
}

av_status av_segment_hnsw_restore(const void *bytes, size_t len, const av_seg_hnsw_info *expect,
                                  const av_opclass *oc, size_t reserve_bytes, av_segment **out, av_err *e)
{
    *out = NULL;
    const char *b = bytes;
    if (bytes == NULL || len < GRAPH_HDR || len % MAXIMUM_ALIGNOF != 0 || len != expect->arena_bytes) {
        av_err_set(e, "invalid_argument", "graph image has a bad size (%zu bytes)", len);
        return AV_ERR;
    }
    if (expect->m < 2 || expect->m > HNSW_MAX_M || expect->dim < 1 || expect->count > DEFAULT_MAX_DOCIDS ||
        expect->max_docid >= DEFAULT_MAX_DOCIDS) {
        av_err_set(e, "invalid_argument", "graph parameters out of range");
        return AV_ERR;
    }
    HnswGraph g;
    memcpy(&g, b, sizeof g);
    int max_level = (int) HnswGetMaxLevel((size_t) expect->m);

    /* 1. Walk the element list: bounded by the expected count (also stops cycles). */
    size_t cap = expect->count > 0 ? expect->count : 1;
    size_t *offs = malloc(cap * sizeof(size_t));
    roaring_bitmap_t *seen = roaring_bitmap_create();
    av_status st = AV_ERR;
    const char *why = NULL;
    size_t n = 0, n_tids = 0;
    img_region *regs = NULL;
    size_t n_regs = 0, cap_regs = 0;
    if (offs == NULL || seen == NULL) {
        why = "out of memory";
        goto done;
    }
    size_t cur = 0;
    if (!img_ptr(g.head.relptr.relptr_off, len, sizeof(HnswElementData), &cur)) {
        why = "head pointer out of bounds";
        goto done;
    }
    while (cur != SIZE_MAX) {
        if (n >= expect->count) {
            why = "more elements than recorded (or a cycle)";
            goto done;
        }
        offs[n++] = cur;
        const HnswElementData *el = (const HnswElementData *) (b + cur);
        if (!img_ptr(el->next.relptr.relptr_off, len, sizeof(HnswElementData), &cur)) {
            why = "element link out of bounds";
            goto done;
        }
    }
    qsort(offs, n, sizeof(size_t), cmp_size);
    for (size_t i = 1; i < n; i++)
        if (offs[i] == offs[i - 1] || offs[i] - offs[i - 1] < sizeof(HnswElementData)) {
            why = "overlapping elements";
            goto done;
        }

    /* 2. Each element: docids, level, value, neighbour lists. */
    for (size_t i = 0; i < n; i++) {
        const HnswElementData *el = (const HnswElementData *) (b + offs[i]);
        if (!img_region_add(&regs, &n_regs, &cap_regs, offs[i], sizeof(HnswElementData))) {
            why = "out of memory";
            goto done;
        }
        if (el->heaptidsLength < 1 || el->heaptidsLength > HNSW_HEAPTIDS || el->level > max_level ||
            el->deleted != 0) {
            why = "element header out of range";
            goto done;
        }
        for (int t = 0; t < el->heaptidsLength; t++) {
            uint32_t id = av_tid_to_docid(&el->heaptids[t]);
            if (id > expect->max_docid || roaring_bitmap_contains(seen, id)) {
                why = "docid out of range or duplicated";
                goto done;
            }
            roaring_bitmap_add(seen, id);
            n_tids++;
        }
        size_t voff = 0;
        if (!img_ptr(el->value.relptr.relptr_off, len, 8, &voff) ||
            !img_value_ok(b, len, voff, expect->type, expect->dim) ||
            !img_region_add(&regs, &n_regs, &cap_regs, voff, VARSIZE(b + voff))) {
            why = "element value malformed";
            goto done;
        }
        size_t noff = 0;
        if (!img_ptr(el->neighbors.relptr.relptr_off, len,
                     sizeof(HnswNeighborArrayPtr) * (size_t) (el->level + 1), &noff) ||
            noff == SIZE_MAX ||
            !img_region_add(&regs, &n_regs, &cap_regs, noff,
                            sizeof(HnswNeighborArrayPtr) * (size_t) (el->level + 1))) {
            why = "neighbour table out of bounds";
            goto done;
        }
        const HnswNeighborArrayPtr *lists = (const HnswNeighborArrayPtr *) (b + noff);
        for (int lc = 0; lc <= el->level; lc++) {
            int lm = HnswGetLayerM(expect->m, lc);
            size_t aoff = 0;
            if (!img_ptr(lists[lc].relptr.relptr_off, len, HNSW_NEIGHBOR_ARRAY_SIZE((size_t) lm), &aoff) ||
                aoff == SIZE_MAX ||
                !img_region_add(&regs, &n_regs, &cap_regs, aoff, HNSW_NEIGHBOR_ARRAY_SIZE((size_t) lm))) {
                why = "neighbour list out of bounds";
                goto done;
            }
            const HnswNeighborArray *na = (const HnswNeighborArray *) (b + aoff);
            if (na->length < 0 || na->length > lm) {
                why = "neighbour count out of range";
                goto done;
            }
            for (int j = 0; j < na->length; j++) {
                size_t to = 0;
                if (!img_ptr(na->items[j].element.relptr.relptr_off, len, sizeof(HnswElementData), &to) ||
                    to == SIZE_MAX || !img_is_element(offs, n, to) ||
                    ((const HnswElementData *) (b + to))->level < lc) {
                    why = "neighbour is not a valid element of that layer";
                    goto done;
                }
            }
        }
    }
    if (n_tids != expect->count) {
        why = "docid count differs from the header";
        goto done;
    }
    if (n_regs > 1)
        qsort(regs, n_regs, sizeof *regs, cmp_region);
    for (size_t i = 1; i < n_regs; i++)
        if (regs[i].lo < regs[i - 1].hi) {
            why = "structures overlap";
            goto done;
        }
    size_t ep = 0;
    if (!img_ptr(g.entryPoint.relptr.relptr_off, len, sizeof(HnswElementData), &ep) ||
        (n == 0) != (ep == SIZE_MAX) || (ep != SIZE_MAX && !img_is_element(offs, n, ep))) {
        why = "entry point is not an element";
        goto done;
    }

    /* 3. Valid: copy into a writable arena and rebuild the docid table. */
    {
        av_segment *s = NULL;
        size_t reserve = reserve_bytes > len + GRAPH_HDR ? reserve_bytes : len + GRAPH_HDR;
        if (av_segment_create_hnsw(NULL, expect->type, expect->dim, oc, expect->m, expect->ef_construction,
                                   reserve, &s, e) != AV_OK)
            goto done_err;
        char *base = av_arena_file_base(s->data);
        if ((char *) s->graph != base ||
            (len > GRAPH_HDR &&
             av_arena_file_alloc(s->data, len - GRAPH_HDR, MAXIMUM_ALIGNOF, e) != base + GRAPH_HDR)) {
            av_segment_destroy(s);
            why = "arena layout mismatch";
            goto done;
        }
        Size total = s->graph->memoryTotal;
        memcpy(base, b, len);
        HnswGraph *gr = s->graph;
        SpinLockInit(&gr->lock);
        LWLockInitialize(&gr->entryLock, hnsw_lock_tranche_id);
        LWLockInitialize(&gr->entryWaitLock, hnsw_lock_tranche_id);
        LWLockInitialize(&gr->allocatorLock, hnsw_lock_tranche_id);
        LWLockInitialize(&gr->flushLock, hnsw_lock_tranche_id);
        gr->memoryTotal = total;
        gr->memoryUsed = (Size) av_arena_file_used(s->data);
        gr->flushed = false;
        if (expect->count > 0 &&
            av_arena_file_ensure(s->table, ((size_t) expect->max_docid + 1) * sizeof(uint64_t), e) != AV_OK) {
            av_segment_destroy(s);
            goto done_err;
        }
        uint64_t *tbl = av_arena_file_base(s->table);
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            HnswElementData *el = (HnswElementData *) (base + offs[i]);
            LWLockInitialize(&el->lock, hnsw_lock_tranche_id);
            uint64_t voff = (uint64_t) (el->value.relptr.relptr_off - 1);
            for (int t = 0; t < el->heaptidsLength; t++) {
                uint32_t id = av_tid_to_docid(&el->heaptids[t]);
                tbl[id] = voff;
                if (!any || id < s->min_docid)
                    s->min_docid = id;
                if (!any || id > s->max_docid)
                    s->max_docid = id;
                any = true;
            }
        }
        s->has_docid_range = any;
        atomic_store_explicit(&s->table_len, any ? (size_t) s->max_docid + 1 : 0, memory_order_release);
        atomic_store_explicit(&s->count, n_tids, memory_order_release);
        *out = s;
        st = AV_OK;
    }
done:
    if (st != AV_OK && why != NULL)
        av_err_set(e, "invalid_argument", "graph image rejected: %s", why);
done_err:
    free(offs);
    free(regs);
    roaring_bitmap_free(seen);
    return st;
}

typedef struct {
    const av_segment *g;
    const av_segment *flat;
    size_t mismatches;
} verify_ctx;

static bool verify_cb(uint32_t id, void *p)
{
    verify_ctx *v = p;
    const void *raw = av_segment_get(v->flat, (av_docid) id);
    const char *stored = av_segment_get(v->g, (av_docid) id);
    if (stored == NULL)
        return true; /* not in the graph: nothing to verify */
    if (raw == NULL) {
        v->mismatches++;
        return true;
    }
    Datum in = PointerGetDatum(raw);
    Datum value;
    bool isnull = false;
    HnswSupport supp = v->g->support;
    if (!HnswFormIndexValue(&value, &in, &isnull, v->g->typeInfo, &supp)) {
        v->mismatches++;
        return true;
    }
    const char *want = DatumGetPointer(value);
    if (VARSIZE_ANY(want) != VARSIZE_ANY(stored) || memcmp(want, stored, VARSIZE_ANY(want)) != 0)
        v->mismatches++;
    if (want != raw)
        pfree((void *) want);
    return true;
}

av_status av_segment_hnsw_verify(const av_segment *g, const av_segment *flat, const roaring_bitmap_t *live,
                                 size_t *mismatches, av_err *e)
{
    *mismatches = 0;
    if (g == NULL || g->kind != AV_SEG_HNSW || flat == NULL || live == NULL)
        return AV_OK;
    verify_ctx v = {g, flat, 0};
    MemoryContext old = CurrentMemoryContext;
    MemoryContext volatile ctx = NULL;
    volatile av_status st = AV_OK;
    PG_TRY();
    {
        ctx = AllocSetContextCreate(NULL, "hnsw_verify", ALLOCSET_DEFAULT_SIZES);
        MemoryContextSwitchTo(ctx);
        roaring_iterate(live, verify_cb, &v);
    }
    PG_CATCH();
    {
        av_err_set(e, "internal", "hnsw verify: %s", av_pg_error_data()->message);
        FlushErrorState();
        st = AV_ERR;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(old);
    if (ctx != NULL)
        MemoryContextDelete(ctx);
    *mismatches = v.mismatches;
    return st;
}

roaring_bitmap_t *av_segment_docids(const av_segment *s)
{
    roaring_bitmap_t *r = roaring_bitmap_create();
    if (r == NULL || s == NULL)
        return r;
    size_t n = atomic_load_explicit(&s->table_len, memory_order_acquire);
    const uint64_t *tbl = av_arena_file_base(s->table);
    for (size_t i = 0; i < n; i++)
        if (tbl[i] != 0)
            roaring_bitmap_add(r, (uint32_t) i);
    return r;
}
