/*
 * test_hnsw.c (D0 spike part, decision Q3): prove that pgvector's in-memory HNSW
 * graph code, copied unchanged, works as Arkivector's index:
 *   1. build 10k x 128-d vectors in an mmapped arena (vector_l2_ops), recall@10 >= 0.95
 *   2. copy the arena to a file, mmap it PROT_READ at a different address, search in
 *      read-only lock mode: identical results (relptr position independence)
 *   3. 4 inserters + 4 searchers concurrently on a fresh arena: no crash, recall >= 0.95
 *      (run under the tsan preset for the data-race check)
 *   4. the insert path reports "seal needed" at pgvector's memory-full point
 *
 * av_spike_insert() is the prototype of av_hnsw_insert (PLAN_DAEMON §8.2): the steps of
 * pgvector's InsertTuple (hnswbuild.c) with "flush to disk" replaced by "seal needed".
 * Duration of the concurrency test: AV_HNSW_SPIKE_SECONDS (default 3).
 */
#include "postgres.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "fmgr.h"
#include "hnsw.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"
#include "varatt.h"
#include "vector.h"

#include "greatest.h"

PGDLLEXPORT Datum vector_l2_squared_distance(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum vector_dims(PG_FUNCTION_ARGS);

#define DIM         128
#define NVEC        10000
#define NQUERY      200
#define K           10
#define ARENA_BYTES ((Size) 256 * 1024 * 1024)

typedef enum { SPIKE_OK, SPIKE_SEAL_NEEDED } spike_result;

typedef struct {
    char *base;
    HnswGraph *graph;
    HnswMemBuildState bs;
    FmgrInfo procinfo;
    HnswTypeInfo typeInfo;
} spike_index;

static Vector **vecs;    /* NVEC + extra for concurrency test */
static Vector **queries; /* NQUERY */
static int nvecs_total;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

static double gauss(unsigned int *seed)
{
    double u1 = ((double) rand_r(seed) + 1.0) / ((double) RAND_MAX + 2.0);
    double u2 = ((double) rand_r(seed) + 1.0) / ((double) RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/*
 * Embedding-like data: real embeddings have a low intrinsic dimension, so vectors
 * are x = A z + noise with z ~ N(0, I_16) and a fixed random 128x16 matrix A.
 * (Uniform random 128-d data has full intrinsic dimension; its recall is printed
 * for information only.)
 */
#define INTRINSIC 16
static float proj[DIM][INTRINSIC];

static Vector *make_vector_kind(unsigned int *seed, bool uniform)
{
    Vector *v = malloc(VECTOR_SIZE(DIM));
    SET_VARSIZE(v, VECTOR_SIZE(DIM));
    v->dim = DIM;
    v->unused = 0;
    if (uniform) {
        for (int i = 0; i < DIM; i++)
            v->x[i] = (float) rand_r(seed) / (float) RAND_MAX;
        return v;
    }
    double z[INTRINSIC];
    for (int j = 0; j < INTRINSIC; j++)
        z[j] = gauss(seed);
    for (int i = 0; i < DIM; i++) {
        double acc = 0;
        for (int j = 0; j < INTRINSIC; j++)
            acc += (double) proj[i][j] * z[j];
        v->x[i] = (float) (acc + 0.01 * gauss(seed));
    }
    return v;
}

static bool use_uniform = false;

static Vector *make_vector(unsigned int *seed)
{
    return make_vector_kind(seed, use_uniform);
}

static void init_data(void)
{
    if (vecs != NULL)
        return;
    unsigned int seed = 42;
    for (int i = 0; i < DIM; i++)
        for (int j = 0; j < INTRINSIC; j++)
            proj[i][j] = (float) gauss(&seed);
    nvecs_total = NVEC * 3;
    vecs = malloc(sizeof(Vector *) * (size_t) nvecs_total);
    for (int i = 0; i < nvecs_total; i++)
        vecs[i] = make_vector(&seed);
    queries = malloc(sizeof(Vector *) * NQUERY);
    for (int i = 0; i < NQUERY; i++)
        queries[i] = make_vector(&seed);
}

/* Header of the arena: the graph lives inside it so the whole index relocates as one blob. */
static void spike_init(spike_index *ix, char *base, Size bytes)
{
    memset(ix, 0, sizeof(*ix));
    ix->base = base;
    ix->graph = (HnswGraph *) base;
    AvHnswInitGraph(ix->graph, base, bytes);
    ix->graph->memoryUsed = MAXALIGN(sizeof(HnswGraph));

    av_fmgr_info(&ix->procinfo, vector_l2_squared_distance);
    ix->typeInfo.maxDimensions = HNSW_MAX_DIM;
    ix->typeInfo.dimensions = vector_dims;
    ix->typeInfo.normalize = NULL;
    ix->typeInfo.checkValue = NULL;
    ix->typeInfo.useFsm = false;

    ix->bs.typeInfo = &ix->typeInfo;
    ix->bs.dimensions = DIM;
    ix->bs.m = HNSW_DEFAULT_M;
    ix->bs.efConstruction = HNSW_DEFAULT_EF_CONSTRUCTION;
    ix->bs.support.procinfo = &ix->procinfo;
    ix->bs.support.normprocinfo = NULL;
    ix->bs.support.collation = InvalidOid;
    ix->bs.graph = ix->graph;
    ix->bs.ml = HnswGetMl(ix->bs.m);
    ix->bs.maxLevel = HnswGetMaxLevel(ix->bs.m);
    ix->bs.hnswarea = base;
    AvHnswInitAllocator(&ix->bs.allocator, AvHnswArenaAlloc, &ix->bs);
}

/* Prototype of av_hnsw_insert: pgvector InsertTuple() steps, seal instead of flush. */
static spike_result spike_insert(spike_index *ix, Vector *v, uint32 docid)
{
    HnswGraph *graph = ix->graph;
    HnswMemBuildState *bs = &ix->bs;
    char *base = ix->base;
    Datum value = PointerGetDatum(v);
    ItemPointerData tid;
    Size valueSize;
    Size memoryMargin = 1024 * 1024; /* pgvector: margin when base != NULL */
    HnswElement element;
    Pointer valuePtr;

    av_docid_to_tid(docid, &tid);
    HnswCheckDim(bs->dimensions, bs->typeInfo, bs->support.collation, value);
    valueSize = VARSIZE_ANY(DatumGetPointer(value));

    LWLockAcquire(&graph->allocatorLock, LW_EXCLUSIVE);
    if (add_size(graph->memoryUsed, memoryMargin) >= graph->memoryTotal) {
        LWLockRelease(&graph->allocatorLock);
        return SPIKE_SEAL_NEEDED;
    }
    element = HnswInitElement(base, &tid, bs->m, bs->ml, bs->maxLevel, &bs->allocator);
    valuePtr = HnswAlloc(&bs->allocator, valueSize);
    LWLockRelease(&graph->allocatorLock);

    memcpy(valuePtr, DatumGetPointer(value), valueSize);
    HnswPtrStore(base, element->value, (char *) valuePtr);
    LWLockInitialize(&element->lock, hnsw_lock_tranche_id);
    AvHnswInsertTupleInMemory(bs, element);
    return SPIKE_OK;
}

typedef struct {
    uint32 docid;
    double distance;
} hit;

static int hit_cmp(const void *a, const void *b)
{
    const hit *x = a, *y = b;
    if (x->distance < y->distance)
        return -1;
    if (x->distance > y->distance)
        return 1;
    return (x->docid > y->docid) - (x->docid < y->docid);
}

/* Search: HnswSearchLayer per layer exactly as pgvector's GetScanItems. Returns n hits. */
static int spike_search(char *base, HnswGraph *graph, HnswSupport *support, int m, Vector *query, int ef,
                        hit *out, int k)
{
    HnswQuery q;
    HnswSearchState ss;
    List *ep;
    List *w;
    ListCell *lc2;
    int n = 0;
    bool readonly = av_lwlock_readonly_arena;

    q.value = PointerGetDatum(query);
    HnswInitSearchState(&ss, NULL, false);

    if (!readonly) {
        /* Same handshake as pgvector's InsertTupleInMemory: let a waiting writer in first. */
        LWLockAcquire(&graph->entryWaitLock, LW_EXCLUSIVE);
        LWLockRelease(&graph->entryWaitLock);
        LWLockAcquire(&graph->entryLock, LW_SHARED);
    }
    HnswElement entryPoint = HnswPtrAccess(base, graph->entryPoint);
    if (entryPoint == NULL) {
        if (!readonly)
            LWLockRelease(&graph->entryLock);
        return 0;
    }
    ep = list_make1(HnswEntryCandidate(base, entryPoint, &q, NULL, support, false));
    for (int lc = entryPoint->level; lc >= 1; lc--)
        ep = HnswSearchLayer(base, &q, ep, 1, lc, NULL, support, m, false, NULL, NULL, NULL, true, NULL, &ss);
    w = HnswSearchLayer(base, &q, ep, ef, 0, NULL, support, m, false, NULL, NULL, NULL, true, NULL, &ss);
    if (!readonly)
        LWLockRelease(&graph->entryLock);
    HnswEndSearchState(&ss);

    int total = list_length(w);
    hit *all = palloc(sizeof(hit) * (size_t) (total > 0 ? total : 1));
    foreach (lc2, w) {
        HnswSearchCandidate *sc = lfirst(lc2);
        HnswElement e = HnswPtrAccess(base, sc->element);
        all[n].docid = av_tid_to_docid(&e->heaptids[0]);
        all[n].distance = sc->distance;
        n++;
    }
    qsort(all, (size_t) n, sizeof(hit), hit_cmp);
    if (n > k)
        n = k;
    memcpy(out, all, sizeof(hit) * (size_t) n);
    return n;
}

static double l2sq(const Vector *a, const Vector *b)
{
    double s = 0;
    for (int i = 0; i < DIM; i++) {
        double d = (double) a->x[i] - (double) b->x[i];
        s += d * d;
    }
    return s;
}

static void brute_force(Vector *q, int nbase, hit *out)
{
    hit *all = malloc(sizeof(hit) * (size_t) nbase);
    for (int i = 0; i < nbase; i++) {
        all[i].docid = (uint32) i;
        all[i].distance = l2sq(q, vecs[i]);
    }
    qsort(all, (size_t) nbase, sizeof(hit), hit_cmp);
    memcpy(out, all, sizeof(hit) * K);
    free(all);
}

static double recall_at_k(char *base, HnswGraph *graph, HnswSupport *support, int m, int ef, int nbase,
                          const uint8 *inserted, hit (*results)[K])
{
    MemoryContext ctx = AllocSetContextCreate(NULL, "recall", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(ctx);
    int found = 0, total = 0;
    for (int qi = 0; qi < NQUERY; qi++) {
        hit got[K], truth[K];
        int n = spike_search(base, graph, support, m, queries[qi], ef, got, K);
        if (results != NULL)
            memcpy(results[qi], got, sizeof(got));
        /* brute force over inserted docids only */
        if (inserted == NULL) {
            brute_force(queries[qi], nbase, truth);
        } else {
            hit *all = malloc(sizeof(hit) * (size_t) nbase);
            int na = 0;
            for (int i = 0; i < nbase; i++)
                if (inserted[i]) {
                    all[na].docid = (uint32) i;
                    all[na].distance = l2sq(queries[qi], vecs[i]);
                    na++;
                }
            qsort(all, (size_t) na, sizeof(hit), hit_cmp);
            memcpy(truth, all, sizeof(hit) * K);
            free(all);
        }
        for (int i = 0; i < K; i++) {
            for (int j = 0; j < n; j++)
                if (got[j].docid == truth[i].docid) {
                    found++;
                    break;
                }
            total++;
        }
        MemoryContextReset(ctx);
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(ctx);
    return (double) found / (double) total;
}

static char *map_anon(Size bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static spike_index main_ix;
static bool main_built;
static double insert_ns_per_op;

static void build_main(void)
{
    if (main_built)
        return;
    init_data();
    char *base = map_anon(ARENA_BYTES);
    spike_init(&main_ix, base, ARENA_BYTES);
    MemoryContext tmp = AllocSetContextCreate(NULL, "insert", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(tmp);
    double t0 = now_ns();
    for (int i = 0; i < NVEC; i++) {
        spike_result r = spike_insert(&main_ix, vecs[i], (uint32) i);
        if (r != SPIKE_OK)
            abort();
        MemoryContextReset(tmp);
    }
    insert_ns_per_op = (now_ns() - t0) / NVEC;
    MemoryContextSwitchTo(old);
    MemoryContextDelete(tmp);
    main_built = true;
}

TEST spike_recall(void)
{
    build_main();
    double t0 = now_ns();
    double r40 =
        recall_at_k(main_ix.base, main_ix.graph, &main_ix.bs.support, main_ix.bs.m, 40, NVEC, NULL, NULL);
    double search40 = (now_ns() - t0) / NQUERY;
    double r100 =
        recall_at_k(main_ix.base, main_ix.graph, &main_ix.bs.support, main_ix.bs.m, 100, NVEC, NULL, NULL);
    t0 = now_ns();
    double r200 =
        recall_at_k(main_ix.base, main_ix.graph, &main_ix.bs.support, main_ix.bs.m, 200, NVEC, NULL, NULL);
    double search200 = (now_ns() - t0) / NQUERY;
    fprintf(
        stderr,
        "\n[spike] %d x %d-d, m=%d ef_construction=%d: insert %.0f ns/op, arena %zu bytes (%.0f B/vector, "
        "element struct %zu B)\n"
        "[spike] recall@10 ef_search=40: %.4f  100: %.4f  200: %.4f  (search incl. brute force: %.0f / %.0f "
        "ns/query)\n",
        NVEC, DIM, main_ix.bs.m, main_ix.bs.efConstruction, insert_ns_per_op, main_ix.graph->memoryUsed,
        (double) main_ix.graph->memoryUsed / NVEC, sizeof(HnswElementData), r40, r100, r200, search40,
        search200);
    {
        MemoryContext ctx = AllocSetContextCreate(NULL, "lat", ALLOCSET_DEFAULT_SIZES);
        MemoryContext old = MemoryContextSwitchTo(ctx);
        hit out[K];
        for (int ef = 40; ef <= 200; ef *= 5) {
            double ts = now_ns();
            for (int qi = 0; qi < NQUERY; qi++) {
                (void) spike_search(main_ix.base, main_ix.graph, &main_ix.bs.support, main_ix.bs.m,
                                    queries[qi], ef, out, K);
                MemoryContextReset(ctx);
            }
            fprintf(stderr, "[spike] search only, ef_search=%d: %.0f ns/query\n", ef,
                    (now_ns() - ts) / NQUERY);
        }
        MemoryContextSwitchTo(old);
        MemoryContextDelete(ctx);
    }
    ASSERT(r40 >= 0.95);
    PASS();
}

TEST spike_mmap_relocation(void)
{
    build_main();
    static hit before[NQUERY][K], after[NQUERY][K];
    (void) recall_at_k(main_ix.base, main_ix.graph, &main_ix.bs.support, main_ix.bs.m, 100, NVEC, NULL,
                       before);

    char path[] = "/tmp/av_spike_arenaXXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    Size used = main_ix.graph->memoryUsed;
    Size off = 0;
    while (off < used) {
        ssize_t n = write(fd, main_ix.base + off, used - off);
        ASSERT(n > 0);
        off += (Size) n;
    }
    /* Map read-only; keep the original mapping alive so the address must differ. */
    char *ro = mmap(NULL, used, PROT_READ, MAP_SHARED, fd, 0);
    ASSERT(ro != MAP_FAILED);
    ASSERT(ro != main_ix.base);
    close(fd);
    unlink(path);

    HnswGraph *rograph = (HnswGraph *) ro;
    av_lwlock_readonly_arena = true;
    double r = recall_at_k(ro, rograph, &main_ix.bs.support, main_ix.bs.m, 100, NVEC, NULL, after);
    av_lwlock_readonly_arena = false;
    munmap(ro, used);

    ASSERT(r >= 0.9);
    for (int q = 0; q < NQUERY; q++)
        for (int i = 0; i < K; i++) {
            ASSERT_EQ(before[q][i].docid, after[q][i].docid);
            ASSERT_EQ(before[q][i].distance, after[q][i].distance);
        }
    fprintf(stderr,
            "[spike] relocated read-only search at %p (built at %p): identical results for %d queries\n",
            (void *) ro, (void *) main_ix.base, NQUERY);
    PASS();
}

/* ---- concurrency ---- */
typedef struct {
    spike_index *ix;
    atomic_int *next;
    atomic_bool *stop;
    uint8 *inserted;
    int limit;
    int id;
    long ops;
} worker_arg;

static void *inserter(void *p)
{
    worker_arg *a = p;
    pg_prng_state prng;
    pg_prng_seed(&prng, (uint64) (1000 + a->id));
    av_hnsw_prng = &prng;
    MemoryContext tmp = AllocSetContextCreate(NULL, "ins", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(tmp);
    while (!atomic_load(a->stop)) {
        int i = atomic_fetch_add(a->next, 1);
        if (i >= a->limit)
            break;
        if (spike_insert(a->ix, vecs[i], (uint32) i) != SPIKE_OK)
            break;
        __atomic_store_n(&a->inserted[i], 1, __ATOMIC_RELEASE);
        a->ops++;
        MemoryContextReset(tmp);
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(tmp);
    av_hnsw_prng = NULL;
    return NULL;
}

static void *searcher(void *p)
{
    worker_arg *a = p;
    MemoryContext tmp = AllocSetContextCreate(NULL, "srch", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(tmp);
    unsigned int seed = (unsigned int) a->id;
    while (!atomic_load(a->stop)) {
        hit out[K];
        int qi = (int) (rand_r(&seed) % NQUERY);
        int n =
            spike_search(a->ix->base, a->ix->graph, &a->ix->bs.support, a->ix->bs.m, queries[qi], 40, out, K);
        for (int i = 1; i < n; i++)
            if (out[i].distance < out[i - 1].distance)
                abort();
        a->ops++;
        MemoryContextReset(tmp);
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(tmp);
    return NULL;
}

TEST spike_concurrent_insert_search(void)
{
    init_data();
    const char *env = getenv("AV_HNSW_SPIKE_SECONDS");
    int seconds = env ? atoi(env) : 3;
    spike_index ix;
    char *base = map_anon(ARENA_BYTES);
    ASSERT(base != NULL);
    spike_init(&ix, base, ARENA_BYTES);
    atomic_int next = 0;
    atomic_bool stop = false;
    uint8 *inserted = calloc((size_t) nvecs_total, 1);
    pthread_t th[8];
    worker_arg args[8];
    for (int i = 0; i < 8; i++) {
        args[i] = (worker_arg){.ix = &ix,
                               .next = &next,
                               .stop = &stop,
                               .inserted = inserted,
                               .limit = nvecs_total,
                               .id = i,
                               .ops = 0};
        pthread_create(&th[i], NULL, i < 4 ? inserter : searcher, &args[i]);
    }
    struct timespec ts = {.tv_sec = seconds, .tv_nsec = 0};
    for (int s = 0; s < seconds * 10; s++) {
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        nanosleep(&tick, NULL);
        if (atomic_load(&next) >= nvecs_total)
            break;
    }
    (void) ts;
    atomic_store(&stop, true);
    long ins = 0, srch = 0;
    for (int i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
        if (i < 4)
            ins += args[i].ops;
        else
            srch += args[i].ops;
    }
    int nbase = atomic_load(&next);
    if (nbase > nvecs_total)
        nbase = nvecs_total;
    double r = recall_at_k(ix.base, ix.graph, &ix.bs.support, ix.bs.m, 200, nbase, inserted, NULL);
    fprintf(stderr,
            "[spike] concurrent %ds: %ld inserts, %ld searches; recall@10 (ef 200) over %ld vectors: %.4f\n",
            seconds, ins, srch, ins, r);
    munmap(base, ARENA_BYTES);
    free(inserted);
    ASSERT(ins > 200);
    ASSERT(r >= 0.95);
    PASS();
}

TEST spike_seal_point(void)
{
    init_data();
    const Size bytes = (Size) 4 * 1024 * 1024;
    char *base = map_anon(bytes);
    spike_index ix;
    spike_init(&ix, base, bytes);
    MemoryContext tmp = AllocSetContextCreate(NULL, "seal", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(tmp);
    int n = 0;
    spike_result r = SPIKE_OK;
    while (n < nvecs_total && (r = spike_insert(&ix, vecs[n], (uint32) n)) == SPIKE_OK) {
        n++;
        MemoryContextReset(tmp);
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(tmp);
    ASSERT_EQ(SPIKE_SEAL_NEEDED, r);
    ASSERT(n > 100);
    /* pgvector's condition: memoryUsed + margin >= memoryTotal, and never past the end */
    ASSERT(ix.graph->memoryUsed + 1024 * 1024 >= ix.graph->memoryTotal);
    ASSERT(ix.graph->memoryUsed <= ix.graph->memoryTotal);
    fprintf(stderr, "[spike] seal needed after %d vectors in a %zu-byte arena (memoryUsed %zu)\n", n, bytes,
            ix.graph->memoryUsed);
    double rec = recall_at_k(base, ix.graph, &ix.bs.support, ix.bs.m, 200, n, NULL, NULL);
    ASSERT(rec >= 0.95);
    munmap(base, bytes);
    PASS();
}

/* Informational: uniform random data (worst case for any graph index). */
TEST spike_uniform_info(void)
{
    if (getenv("AV_HNSW_SPIKE_UNIFORM") == NULL)
        SKIPm("set AV_HNSW_SPIKE_UNIFORM=1");
    unsigned int seed = 7;
    for (int i = 0; i < NVEC; i++)
        vecs[i] = make_vector_kind(&seed, true);
    for (int i = 0; i < NQUERY; i++)
        queries[i] = make_vector_kind(&seed, true);
    main_built = false;
    build_main();
    for (int ef = 40; ef <= 800; ef *= 2) {
        double r =
            recall_at_k(main_ix.base, main_ix.graph, &main_ix.bs.support, main_ix.bs.m, ef, NVEC, NULL, NULL);
        fprintf(stderr, "[spike] uniform 128-d recall@10 ef_search=%d: %.4f\n", ef, r);
    }
    PASS();
}

SUITE(hnsw)
{
    RUN_TEST(spike_recall);
    RUN_TEST(spike_mmap_relocation);
    RUN_TEST(spike_concurrent_insert_search);
    RUN_TEST(spike_seal_point);
    RUN_TEST(spike_uniform_info);
}
