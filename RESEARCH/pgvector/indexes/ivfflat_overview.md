# IVFFlat Index Overview in pgvector

## Table of Contents
1. [Introduction & Algorithm Theory](#1-introduction--algorithm-theory)
   - [Inverted File Flat Architecture](#inverted-file-flat-architecture)
   - [Voronoi Cell Partitioning via k-means](#voronoi-cell-partitioning-via-k-means)
   - [Two-Phase ANN Query Execution](#two-phase-ann-query-execution)
   - [The `lists` Parameter (Cluster Count)](#the-lists-parameter-cluster-count)
   - [The `ivfflat.probes` Parameter (Search Breadth)](#the-ivfflatprobes-parameter-search-breadth)
   - [Recall vs. Latency Tradeoff Formulation](#recall-vs-latency-tradeoff-formulation)
2. [PostgreSQL Access Method (AM) Integration](#2-postgresql-access-method-am-integration)
   - [`IndexAmRoutine` Registration](#indexamroutine-registration)
   - [Detailed AM Callback Comparison: IVFFlat vs. HNSW](#detailed-am-callback-comparison-ivfflat-vs-hnsw)
   - [Operator Classes and Distance Strategies](#operator-classes-and-distance-strategies)
3. [Physical Storage Layout & Core Data Structures](#3-physical-storage-layout--core-data-structures)
   - [Page Organization (Metapage, List Pages, Entry Pages)](#page-organization-metapage-list-pages-entry-pages)
   - [`IvfflatMetaPageData` (Block 0)](#ivfflatmetapagedata-block-0)
   - [`IvfflatPageOpaqueData` (Special Pointer)](#ivfflatpageopaquedata-special-pointer)
   - [`IvfflatListData` (Centroid & Chain Pointers)](#ivfflatlistdata-centroid--chain-pointers)
   - [`IvfflatScanList` & `IvfflatScanOpaqueData`](#ivfflatscanlist--ivfflatscanopaquedata)
   - [Vector In-Memory Containers (`VectorArrayData`)](#vector-in-memory-containers-vectorarraydata)
   - [Polymorphic Type Handling (`IvfflatTypeInfo`)](#polymorphic-type-handling-ivfflattypeinfo)
4. [GUC Parameters & Reloptions](#4-guc-parameters--reloptions)
   - [`ivfflat.probes`](#ivfflatprobes)
   - [`ivfflat.iterative_scan`](#ivfflatiterative_scan)
   - [`ivfflat.max_probes`](#ivfflatmax_probes)
   - [Storage Parameter: `lists`](#storage-parameter-lists)
5. [Architectural Comparison: IVF-Flat vs. IVF-PQ](#5-architectural-comparison-ivf-flat-vs-ivf-pq)
   - [How IVF-PQ Operates](#how-ivf-pq-operates)
   - [Why pgvector Chose IVF-Flat](#why-pgvector-chose-ivf-flat)
   - [Hardware Acceleration & System Synergy](#hardware-acceleration--system-synergy)

---

## 1. Introduction & Algorithm Theory

The **IVFFlat** (Inverted File Flat) index is an approximate nearest neighbor (ANN) search index designed for high-dimensional vector spaces. Originating in large-scale computer vision and information retrieval literature (Sivic & Zisserman 2003, Jégou et al. 2011), IVFFlat adapts the inverted index concept—widely celebrated in full-text search engines—to continuous geometric vector domains.

```
                  High-Dimensional Vector Space Partitioning
       +-------------------------------------------------------------+
       |                                                             |
       |         Voronoi Cell C1                   Voronoi Cell C2   |
       |        * (x1)                            * (x4)             |
       |              X Centroid 1                         X Centroid 2
       |        * (x2)        * (x3)               * (x5)            |
       |                                                             |
       | - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - |
       |                                                             |
       |         Voronoi Cell C3                   Voronoi Cell C4   |
       |                            Query (q)                        |
       |                               ?                             |
       |              X Centroid 3       \                 X Centroid 4
       |        * (x6)                    --> Probe C3     * (x8)    |
       |                        * (x7)                               |
       +-------------------------------------------------------------+
```

### Inverted File Flat Architecture
In text search engines, an inverted index maps words to posting lists of document IDs containing those words. In continuous vector spaces $\mathbb{R}^d$, continuous coordinate values never match exactly. IVFFlat discretizes continuous space into $K$ discrete geometric partitions known as **Voronoi cells** $\mathcal{V}_1, \mathcal{V}_2, \dots, \mathcal{V}_K$, parameterized by centroid vectors $\mathbf{c}_1, \mathbf{c}_2, \dots, \mathbf{c}_K \in \mathbb{R}^d$.

Each Voronoi cell corresponds to an **inverted list** (posting list):
$$\mathcal{L}_i = \{ (\mathbf{x}_j, \text{TID}_j) \mid \arg\min_{k} \|\mathbf{x}_j - \mathbf{c}_k\| = i \}$$

The term **Flat** denotes that the indexed vectors $\mathbf{x}_j$ are preserved in full precision within each posting list on disk, rather than compressed using lossy quantization (such as Product Quantization).

### Voronoi Cell Partitioning via k-means
During index construction, a representative sample of vectors is gathered from the table. The index runs $k$-means clustering on this sample to establish the $K$ centroids:
$$\mathcal{V}_i = \{ \mathbf{x} \in \mathbb{R}^d \mid \mathcal{D}(\mathbf{x}, \mathbf{c}_i) \le \mathcal{D}(\mathbf{x}, \mathbf{c}_j), \; \forall j \ne i \}$$
where $\mathcal{D}(\cdot, \cdot)$ is the distance metric (Euclidean distance, cosine distance, or inner product). Every vector in the dataset is then assigned to the inverted list of its closest centroid.

### Two-Phase ANN Query Execution
When executing an approximate nearest neighbor query with query vector $\mathbf{q}$:
1. **Centroid Routing (Coarse Quantization Phase):** The engine computes the distance between $\mathbf{q}$ and all $K$ centroids:
   $$\mathcal{D}(\mathbf{q}, \mathbf{c}_i) \quad \forall i \in \{1, \dots, K\}$$
   The centroids are sorted in ascending order of distance. The top $P$ closest centroids are selected, where $P = \text{probes}$.
2. **Posting List Scan (Fine Exhaustive Search Phase):** The engine visits only the $P$ inverted lists corresponding to the chosen centroids. For every vector $\mathbf{x} \in \bigcup_{m=1}^P \mathcal{L}_{\pi(m)}$, the exact distance $\mathcal{D}(\mathbf{q}, \mathbf{x})$ is evaluated. The candidates are sorted and returned.

### The `lists` Parameter (Cluster Count)
The number of inverted lists $K$ is configured at index creation via `WITH (lists = ...)`.
- **Default:** `100` (`IVFFLAT_DEFAULT_LISTS`)
- **Minimum:** `1` (`IVFFLAT_MIN_LISTS`)
- **Maximum:** `32768` (`IVFFLAT_MAX_LISTS`)

**Heuristic Guidelines:**
- For datasets up to 1,000,000 rows: $\text{lists} \approx \frac{\text{rows}}{1000}$
- For datasets exceeding 1,000,000 rows: $\text{lists} \approx \sqrt{\text{rows}}$
- If `lists` is set too small, posting lists become large, reducing query speedup towards a sequential scan.
- If `lists` is set too large, centroid search takes longer, cluster sample sizes shrink, and clusters risk becoming empty or underpopulated.

### The `ivfflat.probes` Parameter (Search Breadth)
The number of lists inspected during query execution is controlled by the run-time GUC parameter `ivfflat.probes`.
- **Default:** `1` (`IVFFLAT_DEFAULT_PROBES`)
- **Range:** `1` to `lists` (clamped internally if `probes > lists`)
- With `probes = 1`, only the single closest Voronoi cell is inspected.
- With `probes = lists`, the search inspects all lists, converting the approximate search into an exact exhaustive scan (100% recall, but slower than a sequential scan due to random I/O).

### Recall vs. Latency Tradeoff Formulation
Let $N$ be the total number of indexed vectors, $K$ be `lists`, and $P$ be `probes`. Assuming balanced Voronoi partitions, each list contains $\approx \frac{N}{K}$ vectors.

The computational complexity per query decomposes into:
$$\text{Cost}_{\text{query}} = \underbrace{\mathcal{O}(K \cdot d)}_{\text{Centroid routing}} + \underbrace{\mathcal{O}\left( P \cdot \frac{N}{K} \cdot d \right)}_{\text{Posting list evaluation}} + \underbrace{\mathcal{O}\left( \left(P \cdot \frac{N}{K}\right) \log W \right)}_{\text{Tuplesort / Priority queue}}$$
where $d$ is vector dimensionality and $W$ is the number of candidates retained in memory.

Contrast this with a sequential table scan:
$$\text{Cost}_{\text{seq}} = \mathcal{O}(N \cdot d)$$

The speedup ratio $\mathcal{S}$ is approximately:
$$\mathcal{S} \approx \frac{N \cdot d}{K \cdot d + \frac{P}{K} N \cdot d} = \frac{1}{\frac{K}{N} + \frac{P}{K}}$$

When $K \approx \sqrt{N}$ and $P \ll K$, the speedup reaches $\mathcal{O}(\frac{\sqrt{N}}{P})$. However, boundary-straddling vectors whose nearest neighbors lie in neighboring Voronoi cells are missed if their centroids are not among the top $P$ probed cells. Empirical recall follows an asymptotic curve approaching 1.0 as $P \to K$:

```
Recall
1.0 |                                      ............-------
    |                            ...-------
0.8 |                     ..-----
    |               .-----
0.6 |           .--'
    |        .-'
0.4 |      .'
    |    ./
0.2 |   /
    +---+-----------------------------------------------------> Probes (P)
        1        5        10       20       50       K (lists)
```

---

## 2. PostgreSQL Access Method (AM) Integration

IVFFlat registers as a native PostgreSQL index access method (`INDEX_AM_HANDLER`) via `ivfflathandler(PG_FUNCTION_ARGS)`.

### `IndexAmRoutine` Registration
Defined in [ivfflat.c](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfflat.c#L390-L520), the `IndexAmRoutine` struct defines the access method capabilities and function pointers:

```c
static const IndexAmRoutine amroutine = {
    .type = T_IndexAmRoutine,
    .amstrategies = 0,               /* No btree-style comparison strategies (<, <=, =, ...) */
    .amsupport = IVFFLAT_NPROCS,     /* 5 support procedures */
    .amoptsprocnum = 0,
    .amcanorder = false,             /* Cannot satisfy ORDER BY col ASC/DESC */
    .amcanorderbyop = true,          /* CAN satisfy ORDER BY vector <-> target */
    .amcanhash = false,
    .amconsistentequality = false,
    .amconsistentordering = false,
    .amcanbackward = false,          /* Directional scans unsupported */
    .amcanunique = false,            /* Cannot enforce uniqueness */
    .amcanmulticol = false,          /* Single column only */
    .amoptionalkey = true,
    .amsearcharray = false,
    .amsearchnulls = false,          /* NULLs are skipped */
    .amstorage = false,
    .amclusterable = false,
    .ampredlocks = false,
    .amcanparallel = false,          /* Parallel index scans unsupported */
    .amcanbuildparallel = true,      /* Parallel index build supported (PG 17+) */
    .amcaninclude = false,
    .amusemaintenanceworkmem = false,/* Manages internal memory limits */
    .amsummarizing = false,
    .amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL,
    .amkeytype = InvalidOid,

    /* AM Callbacks */
    .ambuild = ivfflatbuild,
    .ambuildempty = ivfflatbuildempty,
    .aminsert = ivfflatinsert,
    .ambulkdelete = ivfflatbulkdelete,
    .amvacuumcleanup = ivfflatvacuumcleanup,
    .amcanreturn = NULL,
    .amcostestimate = ivfflatcostestimate,
    .amoptions = ivfflatoptions,
    .amproperty = ivfflatproperty,
    .ambuildphasename = ivfflatbuildphasename,
    .amvalidate = ivfflatvalidate,
    .ambeginscan = ivfflatbeginscan,
    .amrescan = ivfflatrescan,
    .amgettuple = ivfflatgettuple,
    .amgetbitmap = NULL,
    .amendscan = ivfflatendscan
};
```

### Detailed AM Callback Comparison: IVFFlat vs. HNSW

| Capability / Callback | IVFFlat (`ivfflat.c`) | HNSW (`hnsw.c`) | Architectural Consequence |
|---|---|---|---|
| **`amcanorderbyop`** | `true` | `true` | Both support distance ordering (`<->`, `<#>`, `<=>`). |
| **`amcanbuildparallel`** | `true` | `true` (PG 17+) | IVFFlat distributes heap scans and centroid assignment across parallel workers using `Sharedsort`. |
| **`amcostestimate`** | `ivfflatcostestimate` | `hnswcostestimate` | IVFFlat calculates cost as a direct linear fraction: `ratio = probes / lists`. Adjusts random-to-sequential I/O ratio (0.5). |
| **`ambuild`** | `ivfflatbuild` | `hnswbuild` | IVFFlat samples data, computes $k$-means centroids, assigns all tuples via Tuplesort, then loads pages sequentially. HNSW builds in-memory graphs with M-nearest-neighbor updates. |
| **`ambuildempty`** | `ivfflatbuildempty` | `hnswbuildempty` | Both write an empty metapage to `INIT_FORKNUM` for unlogged tables. |
| **`aminsert`** | `ivfflatinsert` | `hnswinsert` | IVFFlat performs an $\mathcal{O}(K)$ scan of centroid list pages, then appends to the target posting chain. HNSW executes a greedy graph walk down layers $L$ to $0$ and updates neighbor arrays. |
| **`ambulkdelete`** | `ivfflatbulkdelete` | `hnswbulkdelete` | IVFFlat scans list pages, then entry pages, compacting with `PageIndexMultiDelete`. HNSW removes node pointers and repairs graph edges. |
| **Memory in Scan** | `work_mem` | `hnsw_ef_search` pairing heaps | IVFFlat uses PostgreSQL `Tuplesortstate` backed by `work_mem` to sort retrieved items. |
| **Snapshot Requirement** | Requires `IsMVCCSnapshot` | Requires `IsMVCCSnapshot` | IVFFlat cannot hold buffer pins across `tuplesort_performsort`, requiring an MVCC snapshot. |

### Operator Classes and Distance Strategies
IVFFlat provides 5 support procedures declared in `catalog/pg_amproc`:

| Procedure Index | Constant | Purpose |
|---|---|---|
| 1 | `IVFFLAT_DISTANCE_PROC` | Scan-time and insert-time distance calculation (e.g., L2 squared distance, negative inner product). |
| 2 | `IVFFLAT_NORM_PROC` | Optional vector normalization procedure (used for cosine ops to ensure unit length). |
| 3 | `IVFFLAT_KMEANS_DISTANCE_PROC` | Triangle-inequality compliant metric for $k$-means clustering (e.g., actual L2 distance, spherical/angular distance). |
| 4 | `IVFFLAT_KMEANS_NORM_PROC` | Optional normalization for unit-sphere spherical $k$-means clustering. |
| 5 | `IVFFLAT_TYPE_INFO_PROC` | Type support metadata: max dimensions, dimensions extractor, vector sizing, and accumulator callbacks. |

---

## 3. Physical Storage Layout & Core Data Structures

An IVFFlat index resides in an ordinary PostgreSQL relation fork (`MAIN_FORKNUM`), divided into 8192-byte buffer pages.

```
+-----------------------------------------------------------------------+
| Block 0: METAPAGE                                                     |
| [ PageHeaderData | IvfflatMetaPageData (magic, version, dims, lists) ]|
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
| Block 1 .. M: LIST PAGES (Centroids)                                  |
| [ PageHeaderData | IvfflatListData 1 | ... | IvfflatPageOpaqueData ]  |
|   - startPage  --> Block X (First Entry Page)                         |
|   - insertPage --> Block Y (Last Entry Page with Free Space)          |
|   - center     --> [ float4, float4, ... ]                            |
+-----------------------------------------------------------------------+
         |                                       |
         | (startPage)                           | (startPage)
         v                                       v
+----------------------------------+   +----------------------------------+
| Block X: ENTRY PAGE (Posting 1)  |   | Block Z: ENTRY PAGE (Posting 2)  |
| [ PageHeaderData ]               |   | [ PageHeaderData ]               |
| Item 1: IndexTuple (TID, Vector) |   | Item 1: IndexTuple (TID, Vector) |
| Item 2: IndexTuple (TID, Vector) |   | Item 2: IndexTuple (TID, Vector) |
| ...                              |   | ...                              |
| Special: nextblkno --> Block X+1 |   | Special: nextblkno = Invalid     |
+----------------------------------+   +----------------------------------+
         |
         v
+----------------------------------+
| Block X+1: ENTRY PAGE            |
| Item 3: IndexTuple (TID, Vector) |
| Special: nextblkno = Invalid     |
+----------------------------------+
```

### Page Organization (Metapage, List Pages, Entry Pages)
1. **Metapage (Block 0):** Always located at `IVFFLAT_METAPAGE_BLKNO` (0). Stores index identity, format version, vector dimensionality, and list count.
2. **List Pages (Block 1 .. $M$):** Begins at `IVFFLAT_HEAD_BLKNO` (1). Each item on a list page is an `IvfflatListData` representing one Voronoi centroid and the state of its inverted list. List pages are linked via `IvfflatPageOpaqueData->nextblkno`.
3. **Entry Pages:** Inverted posting list storage. Each page contains a standard PostgreSQL page header, line pointers (`ItemIdData`), `IndexTupleData` items (containing the heap `ItemPointerData` and vector datum), and an opaque structure linking to the next entry page in the list.

### `IvfflatMetaPageData` (Block 0)
Defined in [ivfflat.h](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfflat.h#L239-L247):

```c
typedef struct IvfflatMetaPageData
{
    uint32      magicNumber;    /* IVFFLAT_MAGIC_NUMBER (0x14FF1A7) */
    uint32      version;        /* IVFFLAT_VERSION (1) */
    uint16      dimensions;     /* Vector dimension count (1..2000) */
    uint16      lists;          /* Total number of inverted lists */
} IvfflatMetaPageData;

typedef IvfflatMetaPageData * IvfflatMetaPage;
```

### `IvfflatPageOpaqueData` (Special Pointer)
Located in the page special space at `PageGetSpecialPointer(page)`:

```c
typedef struct IvfflatPageOpaqueData
{
    BlockNumber nextblkno;      /* Next page in list/entry chain, or InvalidBlockNumber */
    uint16      unused;         /* Alignment padding */
    uint16      page_id;        /* IVFFLAT_PAGE_ID (0xFF84) verification mark */
} IvfflatPageOpaqueData;

typedef IvfflatPageOpaqueData * IvfflatPageOpaque;
```

### `IvfflatListData` (Centroid & Chain Pointers)
Stored as an index item on list pages via `PageAddItem`:

```c
typedef struct IvfflatListData
{
    BlockNumber startPage;      /* First entry page for this inverted list */
    BlockNumber insertPage;     /* Entry page currently targeted for appending new tuples */
    Vector      center;         /* Variable-length centroid vector */
} IvfflatListData;

typedef IvfflatListData * IvfflatList;
```
The total size of this structure is dynamic:
```c
#define IVFFLAT_LIST_SIZE(size) add_size(offsetof(IvfflatListData, center), size)
```

### `IvfflatScanList` & `IvfflatScanOpaqueData`
The scan state tracking candidate centroids and execution context:

```c
typedef struct IvfflatScanList
{
    pairingheap_node ph_node;   /* Node embedded in bounded priority queue */
    BlockNumber      startPage; /* First page of posting list */
    double           distance;  /* Distance from query vector to list centroid */
} IvfflatScanList;
```

The scan context allocated in `ivfflatbeginscan`:
```c
typedef struct IvfflatScanOpaqueData
{
    const IvfflatTypeInfo *typeInfo;
    int                    probes;         /* Active number of probes per batch */
    int                    maxProbes;      /* Upper limit for iterative scan */
    int                    dimensions;     /* Vector dimensions */
    bool                   first;          /* True before first tuple returned */
    Datum                  value;          /* Detoasted/normalized query vector */
    MemoryContext          tmpCtx;         /* Memory context for per-scan allocations */

    /* Sorting and candidate accumulation */
    Tuplesortstate        *sortstate;      /* Postgres sorting engine */
    TupleDesc              tupdesc;        /* Tuple descriptor: (distance, heaptid) */
    TupleTableSlot        *vslot;          /* Virtual slot for feeding sortstate */
    TupleTableSlot        *mslot;          /* Minimal slot for fetching results */
    BufferAccessStrategy   bas;            /* BAS_BULKREAD shared buffer ring */

    /* Distance support */
    FmgrInfo              *procinfo;
    FmgrInfo              *normprocinfo;
    Oid                    collation;
    Datum (*distfunc) (FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2);

    /* Centroid queue */
    pairingheap           *listQueue;      /* Max-heap of nearest centroids */
    BlockNumber           *listPages;      /* Array of startPages sorted by distance ASC */
    int                    listIndex;      /* Current index into listPages */
    IvfflatScanList       *lists;          /* Flat storage backing listQueue */
} IvfflatScanOpaqueData;
```

### Vector In-Memory Containers (`VectorArrayData`)
During index construction and clustering, pgvector uses a packed contiguous array:

```c
typedef struct VectorArrayData
{
    int         length;         /* Number of active vectors in array */
    int         maxlen;         /* Maximum allocated capacity */
    int         dim;            /* Dimensions per vector */
    Size        itemsize;       /* MAXALIGNed size of each vector struct */
    char       *items;          /* Contiguous memory buffer */
} VectorArrayData;

typedef VectorArrayData * VectorArray;
```
Access is performed via inline pointer arithmetic to avoid per-element allocations:
```c
static inline Pointer
VectorArrayGet(VectorArray arr, int offset)
{
    return ((char *) arr->items) + (offset * arr->itemsize);
}
```

### Polymorphic Type Handling (`IvfflatTypeInfo`)
IVFFlat supports `vector`, `halfvec` (16-bit float), and `bit` (binary vectors) through function pointer tables returned by `IvfflatGetTypeInfo`:

```c
typedef struct IvfflatTypeInfo
{
    int         maxDimensions;
    Datum       (*dimensions) (PG_FUNCTION_ARGS);
    Datum       (*normalize) (PG_FUNCTION_ARGS);
    Size        (*itemSize) (int dimensions);
    void        (*updateCenter) (Pointer v, int dimensions, float *x);
    void        (*sumCenter) (Pointer v, int dimensions, float *x);
} IvfflatTypeInfo;
```

---

## 4. GUC Parameters & Reloptions

| Parameter | Type | Default | Scope | Description |
|---|---|---|---|---|
| `ivfflat.probes` | `integer` | `1` | Session / User (`PGC_USERSET`) | Number of inverted lists probed during ANN query execution (1 to 32,768). |
| `ivfflat.iterative_scan` | `enum` | `'off'` | Session / User (`PGC_USERSET`) | Mode for iterative scanning: `'off'` or `'relaxed_order'`. |
| `ivfflat.max_probes` | `integer` | `32768` | Session / User (`PGC_USERSET`) | Maximum probe limit when expanding lists in iterative scans. |
| `lists` | `integer` | `100` | Index Option (`reloptions`) | Number of Voronoi cells / centroids established during `CREATE INDEX`. |

### `ivfflat.probes`
Defines how many Voronoi cells are searched in parallel. Increasing `probes` enhances recall at the expense of query latency:
```sql
SET ivfflat.probes = 10;
```
When `probes` exceeds the index's `lists`, it is silently clamped to `lists` during `ivfflatbeginscan`.

### `ivfflat.iterative_scan`
When enabled (`SET ivfflat.iterative_scan = relaxed_order;`), pgvector changes its scan engine to allow progressive expansion of probed lists. If a query includes a selective `WHERE` clause or requests more tuples than exist in the initial `probes` lists, the engine transparently loads additional batches of lists up to `ivfflat.max_probes`.

### `ivfflat.max_probes`
Bounds the total number of lists an iterative scan is allowed to explore before terminating.

### Storage Parameter: `lists`
Specified at index creation:
```sql
CREATE INDEX items_embedding_idx ON items 
USING ivfflat (embedding vector_l2_ops) 
WITH (lists = 500);
```

---

## 5. Architectural Comparison: IVF-Flat vs. IVF-PQ

In the broader vector database ecosystem (e.g., Faiss, Milvus), Inverted File indexes frequently pair with **Product Quantization (IVF-PQ)**. It is instructive to examine why pgvector intentionally implemented **IVF-Flat**.

```
                        IVF-Flat vs. IVF-PQ
                        
    IVF-Flat (pgvector)                    IVF-PQ (e.g., Faiss)
+-------------------------+            +-------------------------+
| Query Vector (q)        |            | Query Vector (q)        |
+-------------------------+            +-------------------------+
             |                                      |
             v                                      v
  Centroid Distance Calc                 Centroid Distance Calc
             |                                      |
             v                                      v
+-------------------------+            +-------------------------+
| Exact Vector Read       |            | Quantized Code Read     |
| (Full float4 on disk)   |            | (e.g. 8 bytes per vector|
+-------------------------+            +-------------------------+
             |                                      |
             v                                      v
  Exact Distance Metric                  Asymmetric Distance (ADC)
  (SIMD accelerated)                     (Table lookup / Approx)
             |                                      |
             v                                      v
  No Quantization Error                  Lossy Distance Distortion
  100% Precision in cell                 Candidate re-ranking req.
```

### How IVF-PQ Operates
In IVF-PQ, high-dimensional vectors are divided into $M$ smaller sub-vectors (e.g., 128 dimensions split into 16 sub-vectors of 8 dimensions). Each sub-vector space is clustered into $K^*=256$ centroids, represented by a single byte codebook index.
- A 128-dimensional vector (512 bytes of float32) compresses into 16 bytes.
- Searching candidate lists computes distances via Asymmetric Distance Computation (ADC) using precomputed lookup tables.

### Why pgvector Chose IVF-Flat
1. **Zero Quantization Distortion:** Within probed Voronoi cells, every vector comparison is exact. There is no distance distortion or loss of rank precision among candidates.
2. **PostgreSQL Storage Engine Fit:** PostgreSQL's storage engine operates on 8KB block pages. Tuple headers, null bitmaps, and variable-length datums have inherent overhead. Compressing vectors to 16 bytes yields diminishing returns when heap fetches (`ItemPointerData`) must follow.
3. **Hardware SIMD Efficiency:** pgvector uses optimized AVX-512, AVX2, and ARM NEON SIMD routines. Computing direct floating-point vector distances over flat memory often matches or exceeds the throughput of scattered cache lookups required by PQ codebook tables.
4. **Polymorphic Type Support:** Flat storage maps cleanly to PostgreSQL's native type system for `vector` (float4), `halfvec` (float16), and `bit` (Hamming).
5. **Transactional Integrity & Crash Recovery:** IVF-Flat relies on PostgreSQL's standard `GenericXLog` logging facilities. Codebook management, codebook versioning, and re-quantization upon incremental updates introduce substantial transactional complexity in a relational WAL engine.
