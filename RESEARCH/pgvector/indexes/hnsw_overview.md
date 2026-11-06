# pgvector HNSW Index: Theoretical Foundations and PostgreSQL Access Method Architecture

## Table of Contents
1. [HNSW Algorithm Theory](#1-hnsw-algorithm-theory)
   - [Small World Networks & Skip-List Generalization](#small-world-networks--skip-list-generalization)
   - [Hierarchical Multi-Layer Structure](#hierarchical-multi-layer-structure)
   - [Probabilistic Level Assignment](#probabilistic-level-assignment)
   - [Search & Routing Mechanics](#search--routing-mechanics)
   - [Complexity Analysis: Why Search is $O(\log N)$](#complexity-analysis-why-search-is-olog-n)
   - [Key Hyperparameters: $M$, $M_0$, and $ef\_construction$](#key-hyperparameters-m-m_0-and-ef_construction)
2. [PostgreSQL Access Method (AM) Integration](#2-postgresql-access-method-am-integration)
   - [The Index AM Contract (`IndexAmRoutine`)](#the-index-am-contract-indexamroutine)
   - [Complete Callback Inventory & Analysis](#complete-callback-inventory--analysis)
   - [AM Flags & Operational Capabilities](#am-flags--operational-capabilities)
   - [Operator Class Validation (`hnswvalidate`)](#operator-class-validation-hnswvalidate)
3. [Core Data Structures (`src/hnsw.h`)](#3-core-data-structures-srchnswh)
   - [In-Memory State Representation](#in-memory-state-representation)
   - [Dual Pointer Architecture: Absolute vs Relative Pointers (`relptr`)](#dual-pointer-architecture-absolute-vs-relative-pointers-relptr)
   - [Candidate and Neighbor Priority Structures](#candidate-and-neighbor-priority-structures)
4. [Storage Architecture and Page Layout](#4-storage-architecture-and-page-layout)
   - [Metapage Architecture (Block 0)](#metapage-architecture-block-0)
   - [Page Special Space (`HnswPageOpaqueData`)](#page-special-space-hnswpageopaquedata)
   - [Element Tuple Format (`HnswElementTupleData`)](#element-tuple-format-hnswelementtupledata)
   - [Neighbor Tuple Format (`HnswNeighborTupleData`)](#neighbor-tuple-format-hnswneighbortupledata)
   - [Cross-Page Storage & Stride Layout](#cross-page-storage--stride-layout)
5. [Planner Integration and Cost Estimation (`hnswcostestimate`)](#5-planner-integration-and-cost-estimation-hnswcostestimate)
   - [Costing Model Mathematical Derivation](#costing-model-mathematical-derivation)
   - [Handling Distance-Ordered Scans](#handling-distance-ordered-scans)
6. [GUC and Index Options Configuration](#6-guc-and-index-options-configuration)
   - [Storage Parameters (Reloptions)](#storage-parameters-reloptions)
   - [Runtime GUC Variables](#runtime-guc-variables)
7. [Memory Management Architecture](#7-memory-management-architecture)
   - [Memory Context Hierarchies](#memory-context-hierarchies)
   - [The `HnswAllocator` Abstraction](#the-hnswallocator-abstraction)
   - [Dynamic Shared Memory (DSM) Allocations](#dynamic-shared-memory-dsm-allocations)

---

## 1. HNSW Algorithm Theory

### Small World Networks & Skip-List Generalization
The Hierarchical Navigable Small World (HNSW) graph model, introduced by Malkov and Yashunin (2018), is an approximate nearest neighbor (ANN) search data structure built upon the concepts of Kleinberg's small-world networks and multi-layer skip-lists. 

In low dimensions, geometric partitioning trees (such as k-d trees or ball trees) perform well. However, in high-dimensional vector spaces ($D > 50$), geometric spaces suffer from the "curse of dimensionality": distances concentrate, boundaries blur, and tree traversal degrades to an exhaustive $O(N)$ linear scan.

HNSW addresses this by constructing a hierarchy of proximity graphs $\mathcal{G} = \{G_0, G_1, \dots, G_{L_{max}}\}$ where each layer contains a subset of the dataset vectors:
- **Layer 0 ($G_0$)**: Contains all elements ($N$). The ground graph has high clustering and short-range connections.
- **Layers $1 \dots L_{max}$**: Contain progressively sparser subsets of elements. Edges in higher layers span larger Euclidean/angular distances, effectively acting as high-speed "express lanes" analogous to the upper layers of a skip-list.

```
Layer 3 (Sparse, Express Links)
[Entry Point: v8] ---------------------------------------------> [v42]
       |                                                           |
Layer 2 (Coarser Graph)                                            |
[v8] -----------------------> [v19] ---------------------------> [v42]
  |                             |                                  |
Layer 1 (Intermediate)          |                                  |
[v8] -------> [v12] --------> [v19] --------> [v31] -----------> [v42]
  |             |               |               |                  |
Layer 0 (Dense Ground Layer, All Vertices)                         |
[v1] -> [v8] -> [v12] -> [v15] -> [v19] -> [v24] -> [v31] -> [v37] -> [v42]
```

### Hierarchical Multi-Layer Structure
Routing begins at the highest layer $L_{max}$ at a designated **Entry Point**. Search proceeds greedily: at the current layer, the algorithm examines all neighbors of the current candidate and hops to the neighbor closest to query vector $q$. When no neighbor is closer to $q$ than the current local minimum, search drops down to the next lower layer $l-1$, using the local minimum found at layer $l$ as the new entry point for layer $l-1$.

At Layer 0, the search switches from a purely greedy 1-NN walk to a priority-queue-directed dynamic beam search bounded by candidate pool size $ef$ ($ef\_construction$ during build/insert, $ef\_search$ during query execution).

### Probabilistic Level Assignment
To preserve logarithmic search complexity without maintaining global balance invariants, each newly inserted element $v$ is assigned an intrinsic maximum level $l$ drawn from a decaying geometric distribution:

$$P(\text{level} = l) = \left(1 - e^{-1/m_L}\right) \cdot e^{-l/m_L}$$

In `pgvector/src/hnsw.h` and `pgvector/src/hnswutils.c`, this parameter is defined as:

$$\begin{aligned}
m_L &= \frac{1}{\ln(M)} \\
l &= \lfloor -\ln(\text{uniform}(0, 1)) \cdot m_L \rfloor
\end{aligned}$$

```c
/* In src/hnsw.h */
#define HnswGetMl(m) (1 / log(m))

/* In src/hnswutils.c: HnswInitElement */
double uniform = RandomDouble();
int level = uniform == 0.0 ? maxLevel : (int) (-log(uniform) * ml);

if (level > maxLevel)
    level = maxLevel;
```

With $m_L = 1/\ln(M)$, the expected number of elements at level $l$ decreases by a factor of $M$ for each step up in the hierarchy. The expected maximum level across a dataset of size $N$ is:

$$\mathbb{E}[L_{max}] \approx \log_M(N)$$

### Search & Routing Mechanics
During top-down greedy descent ($lc = L_{max} \dots 1$):
1. Candidate set has size $ef = 1$.
2. The search inspects the outgoing links of current node $c$.
3. If any neighbor $n \in \text{neighbors}(c, lc)$ satisfies $\text{dist}(n, q) < \text{dist}(c, q)$, then $c \leftarrow n$.
4. Loop until no closer neighbor is found (local minimum reached for layer $lc$).
5. Use node $c$ as the entry point for layer $lc - 1$.

At Layer 0 ($lc = 0$):
1. A dynamic list of nearest elements $W$ (maintained as a max-heap of capacity $ef$) and a candidate pool $C$ (maintained as a min-heap) are populated from entry point(s).
2. The nearest element $c \in C$ is extracted.
3. If $\text{dist}(c, q) > \max_{w \in W} \text{dist}(w, q)$, exploration stops.
4. Otherwise, for every neighbor $e$ of $c$, if $e$ has not been visited:
   - Add $e$ to visited set.
   - If $\text{dist}(e, q) < \max_{w \in W} \text{dist}(w, q)$ or $|W| < ef$:
     - Add $e$ to $C$ and $W$.
     - If $|W| > ef$, remove furthest element from $W$.

### Complexity Analysis: Why Search is $O(\log N)$
The multi-layer graph ensures two critical invariant approximations:
1. **Constant hops per upper layer**: At layer $l > 0$, the graph is a scale-free routing network where the expected number of hops to reach the local minimum is bounded by a constant $\mathcal{O}(1)$ independent of $N$.
2. **Logarithmic depth**: The expected number of layers is $\mathcal{O}(\log_M N)$.
3. **Bounded Layer 0 convergence**: Once down at layer 0, the beam search begins in the immediate geometric basin of attraction of $q$, requiring $\mathcal{O}(ef \cdot \log N)$ distance computations.

Thus, the total search complexity scales as:

$$T_{\text{search}} = \mathcal{O}(M \cdot \log N)$$

### Key Hyperparameters: $M$, $M_0$, and $ef\_construction$
- **$M$ (Max Connections per Upper Layer)**: Sets the maximum number of bidirectional edges each vertex can retain in layers $l \ge 1$. Higher values improve recall on clustered data at the cost of memory and indexing time.
- **$M_0$ (Max Connections for Ground Layer 0)**: In Malkov & Yashunin and pgvector, ground layer vertices maintain up to $2M$ connections:
  ```c
  #define HnswGetLayerM(m, layer) (layer == 0 ? (m) * 2 : (m))
  ```
  Doubling connectivity on $G_0$ improves recall substantially because $G_0$ must bridge tight clusters to preserve high graph connectivity.
- **$ef\_construction$**: Size of the dynamic candidate list during graph creation. Controls the exploration breadth during index construction. Must be at least $2M$.

---

## 2. PostgreSQL Access Method (AM) Integration

### The Index AM Contract (`IndexAmRoutine`)
PostgreSQL integrates custom indexing engines via the Index Access Method API (`IndexAmRoutine` defined in `access/amapi.h`). The AM registration function is exported via the `PG_FUNCTION_INFO_V1(hnswhandler)` macro.

When an index operation is invoked (e.g. `CREATE INDEX ... USING hnsw`), the Postgres executor and planner interact with pgvector exclusively through the function pointers populated in `IndexAmRoutine`.

```
                    ┌────────────────────────┐
                    │ PostgreSQL Query Engine │
                    └───────────┬────────────┘
                                │
                        IndexAmRoutine API
                                │
        ┌───────────────────────┼────────────────────────┐
        ▼                       ▼                        ▼
 ┌──────────────┐       ┌──────────────┐         ┌──────────────┐
 │  Lifecycle   │       │ Construction │         │ Query / Scan │
 ├──────────────┤       ├──────────────┤         ├──────────────┤
 │ hnswhandler  │       │ hnswbuild    │         │ hnswbeginscan│
 │ hnswoptions  │       │ hnswbuildempty│        │ hnswrescan   │
 │ hnswvalidate │       │ hnswinsert   │         │ hnswgettuple │
 │ hnswproperty │       │              │         │ hnswendscan  │
 └──────────────┘       └──────────────┘         └──────────────┘
                                │
                        ┌───────┴──────┐
                        ▼              ▼
                 ┌──────────────┐┌──────────────┐
                 │  Vacuum AM   ││ Cost Planner │
                 ├──────────────┤├──────────────┤
                 │hnswbulkdelete││hnswcostest.  │
                 │hnswvacuumcln.││              │
                 └──────────────┘└──────────────┘
```

### Complete Callback Inventory & Analysis

| Callback Function | `IndexAmRoutine` Field | Invocation Context | Description & Semantic Responsibilities |
|:---|:---|:---|:---|
| `hnswbuild` | `ambuild` | `CREATE INDEX` | Populates the index from an existing heap relation via table scan. |
| `hnswbuildempty` | `ambuildempty` | Unlogged tables | Creates the initial empty metapage on the `INIT_FORKNUM` fork. |
| `hnswinsert` | `aminsert` | `INSERT` / `UPDATE` | Inserts a single vector tuple, finding neighbors and WAL logging via GenericXLog. |
| `hnswbulkdelete` | `ambulkdelete` | `VACUUM` | Multi-pass vacuum removing dead heap TIDs and repairing graph connectivity. |
| `hnswvacuumcleanup` | `amvacuumcleanup` | Post-`VACUUM` cleanup | Updates index relation statistics (e.g. block count) and FSM vacuuming. |
| `hnswcostestimate` | `amcostestimate` | Query Optimizer | Estimates startup and total cost, page fetches, and tuple selectivity. |
| `hnswoptions` | `amoptions` | DDL Parsing | Parses reloptions (`m`, `ef_construction`) using `build_reloptions`. |
| `hnswproperty` | `amproperty` | Index inspection | Confirms AM support for `AMPROP_DISTANCE_ORDERABLE`. |
| `hnswbuildphasename`| `ambuildphasename` | Progress reporting | Maps build progress phases (`initializing`, `loading tuples`). |
| `hnswvalidate` | `amvalidate` | Catalog verification | Validates operator classes, signatures, and support function registration. |
| `hnswbeginscan` | `ambeginscan` | Query Execution | Allocates `HnswScanOpaqueData`, search state, prefetch stream, and temporary memory context. |
| `hnswrescan` | `amrescan` | Rescan / Nested Loop | Resets scan iterator state, distance trackers, and candidate queues. |
| `hnswgettuple` | `amgettuple` | Row iteration | Fetches next closest `ItemPointer` matching query ordering. |
| `NULL` | `amgetbitmap` | Bitmap Index Scan | **Explicitly NULL**. HNSW does not support bitmap scans because graph traversal cannot emit un-ordered bulk TIDs efficiently. |
| `hnswendscan` | `amendscan` | Scan termination | Deallocates search state, prefetch stream, and memory contexts. |

### AM Flags & Operational Capabilities

In `hnsw.c`, the `IndexAmRoutine` structure declares exact engine capabilities:

```c
IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

amroutine->amstrategies = 0;              /* No operator strategies (e.g. <, <=, =, >=) */
amroutine->amsupport = HNSW_NPROCS;       /* 3 internal support procs */
amroutine->amoptsprocnum = 0;
amroutine->amcanorder = false;            /* Cannot produce scalar ordering */
amroutine->amcanorderbyop = true;         /* CAN produce ordering by distance operator (<->, <#>, <=>) */
amroutine->amcanbackward = false;         /* Only forward distance scanning */
amroutine->amcanunique = false;           /* Not a unique index */
amroutine->amcanmulticol = false;         /* Single column indexing only */
amroutine->amoptionalkey = true;          /* Scan keys can be omitted */
amroutine->amsearcharray = false;
amroutine->amsearchnulls = false;         /* Nulls are ignored / not indexed */
amroutine->amstorage = false;
amroutine->amclusterable = false;
amroutine->ampredlocks = false;
amroutine->amcanparallel = false;         /* Parallel index scan not supported */
amroutine->amcanbuildparallel = true;     /* Parallel index BUILD is supported */
amroutine->amcaninclude = false;
amroutine->amusemaintenanceworkmem = false;
amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
amroutine->amkeytype = InvalidOid;
```

### Operator Class Validation (`hnswvalidate`)
During database startup or extension installation, PostgreSQL executes `amvalidate` to inspect system catalogs (`pg_amop`, `pg_amproc`, `pg_opclass`, `pg_opfamily`):
1. **Operator Verification**: Ensures all operators have strategy number `1`, specify `AMOP_ORDER`, can sort `FLOAT8OID`, and match binary input signatures `(vector, vector) -> float8`.
2. **Support Function Verification**:
   - `procnum 1` (`HNSW_DISTANCE_PROC`): Computes distance: `(Type, Type) -> float8`. Required.
   - `procnum 2` (`HNSW_NORM_PROC`): Computes vector norm: `(Type) -> float8` (used for cosine distance pre-normalization). Optional.
   - `procnum 3` (`HNSW_TYPE_INFO_PROC`): Returns `HnswTypeInfo` internal pointer. Optional.

---

## 3. Core Data Structures (`src/hnsw.h`)

### In-Memory State Representation
The core in-memory graph is encapsulated in `HnswGraph`, which coordinates all active elements and multi-process concurrency during builds:

```c
typedef struct HnswGraph
{
    /* Graph state */
    slock_t     lock;
    HnswElementPtr head;
    double      indtuples;

    /* Entry state */
    LWLock      entryLock;
    LWLock      entryWaitLock;
    HnswElementPtr entryPoint;

    /* Allocations state */
    LWLock      allocatorLock;
    Size        memoryUsed;
    Size        memoryTotal;

    /* Flushed state */
    LWLock      flushLock;
    bool        flushed;
} HnswGraph;
```

Each graph node is modeled by `HnswElementData`:

```c
struct HnswElementData
{
    HnswElementPtr next;                        /* Singly-linked list of all elements */
    ItemPointerData heaptids[HNSW_HEAPTIDS];    /* Up to 10 heap TIDs for identical vectors */
    uint8       heaptidsLength;                 /* Number of valid heap TIDs */
    uint8       level;                          /* Highest layer assigned to this element */
    uint8       deleted;                        /* Deletion marker */
    uint8       version;                        /* Monotonic version counter (1..15) */
    uint32      hash;                           /* Precomputed hash for visited hash tables */
    HnswNeighborsPtr neighbors;                 /* Pointer to array of neighbor arrays */
    BlockNumber blkno;                          /* Assigned Postgres disk block */
    OffsetNumber offno;                         /* Assigned disk offset number */
    OffsetNumber neighborOffno;                 /* Disk offset of neighbor tuple */
    BlockNumber neighborPage;                   /* Disk block containing neighbor tuple */
    DatumPtr    value;                          /* Pointer to vector datum */
    LWLock      lock;                           /* Concurrency lock for neighbor/tid updates */
};
```

### Dual Pointer Architecture: Absolute vs Relative Pointers (`relptr`)
In standard serial index construction, elements reside in backend-private memory (`graphCtx`), where regular 64-bit virtual memory pointers (`type *ptr`) are valid.

In parallel index builds, multiple worker backends share the graph held inside Dynamic Shared Memory (DSM). Because each worker process maps the DSM segment at an arbitrary base address in its virtual address space, absolute C pointers would cause segmentation faults.

`pgvector` solves this using PostgreSQL's `relptr` (relative pointer) macro suite:
- A relative pointer stores the 64-bit integer offset from the beginning of the shared memory region (`hnswarea`).
- The macro `HnswPtrDeclare` creates a union capable of acting as either a raw pointer or a relative pointer:

```c
#define HnswPtrDeclare(type, relptrtype, ptrtype) \
    relptr_declare(type, relptrtype); \
    typedef union { type *ptr; relptrtype relptr; } ptrtype

HnswPtrDeclare(HnswElementData, HnswElementRelptr, HnswElementPtr);
HnswPtrDeclare(HnswNeighborArray, HnswNeighborArrayRelptr, HnswNeighborArrayPtr);
HnswPtrDeclare(HnswNeighborArrayPtr, HnswNeighborsRelptr, HnswNeighborsPtr);
HnswPtrDeclare(char, DatumRelptr, DatumPtr);
```

Access across either mode is unified via the `base` pointer parameter:
```c
#define HnswPtrAccess(base, hp) \
    ((base) == NULL ? (hp).ptr : relptr_access(base, (hp).relptr))

#define HnswPtrStore(base, hp, value) \
    ((base) == NULL ? (void) ((hp).ptr = (value)) : (void) relptr_store(base, (hp).relptr, value))

#define HnswPtrIsNull(base, hp) \
    ((base) == NULL ? (hp).ptr == NULL : relptr_is_null((hp).relptr))
```

When `base == NULL` (serial build or on-disk insert), zero conversion overhead occurs. When `base != NULL`, pointer arithmetic `(base + offset)` is transparently applied.

### Candidate and Neighbor Priority Structures
Neighbor relationships use compact arrays with inline candidate slots:

```c
typedef struct HnswCandidate
{
    HnswElementPtr element;
    float       distance;
    bool        closer;       /* Caching flag for Algorithm 4 neighbor selection */
} HnswCandidate;

struct HnswNeighborArray
{
    int         length;
    bool        closerSet;
    HnswCandidate items[FLEXIBLE_ARRAY_MEMBER];
};
```

During graph search (`HnswSearchLayer`), candidate nodes are tracked inside pairing heaps:
```c
typedef struct HnswSearchCandidate
{
    pairingheap_node c_node;    /* Node for min-heap C (nearest candidates) */
    pairingheap_node w_node;    /* Node for max-heap W (furthest accepted candidates) */
    HnswElementPtr element;
    double      distance;
} HnswSearchCandidate;
```

---

## 4. Storage Architecture and Page Layout

### Metapage Architecture (Block 0)
Block 0 of any `hnsw` index relation is permanently designated as `HNSW_METAPAGE_BLKNO`. It contains physical metadata and routing anchors:

```c
typedef struct HnswMetaPageData
{
    uint32      magicNumber;       /* 0xA953A953 validation signature */
    uint32      version;           /* HNSW_VERSION (currently 1) */
    uint32      dimensions;        /* Vector dimensions (e.g. 1536) */
    uint16      m;                 /* Max connections per upper layer */
    uint16      efConstruction;    /* Build candidate pool size */
    BlockNumber entryBlkno;        /* Block number of the root entry element */
    OffsetNumber entryOffno;       /* Offset of the root entry element */
    int16       entryLevel;        /* Highest level of the root entry point */
    BlockNumber insertPage;        /* Block number tracking free insertion target */
} HnswMetaPageData;
```

### Page Special Space (`HnswPageOpaqueData`)
Every 8KB page in the index relation reserves a special area at `PageGetSpecialPointer(page)`:

```c
typedef struct HnswPageOpaqueData
{
    BlockNumber nextblkno;         /* Physical forward link forming an index-wide page list */
    uint16      unused;
    uint16      page_id;           /* 0xFF90 identifying HNSW pages */
} HnswPageOpaqueData;
```

All non-metapage pages in the index form a forward-linked linear list (`nextblkno`), allowing `VACUUM` and sequential verification passes to iterate across the entire index without index locks.

### Element Tuple Format (`HnswElementTupleData`)
Vector data and heap identifiers are stored inside `HnswElementTupleData`:

```c
typedef struct HnswElementTupleData
{
    uint8       type;                           /* HNSW_ELEMENT_TUPLE_TYPE = 1 */
    uint8       level;                          /* Max layer for this element */
    uint8       deleted;                        /* 1 if marked deleted by VACUUM, 0 otherwise */
    uint8       version;                        /* 1..15 tuple generation version */
    ItemPointerData heaptids[HNSW_HEAPTIDS];    /* Array of 10 Heap TIDs */
    ItemPointerData neighbortid;                /* ItemPointer to corresponding neighbor tuple */
    uint16      unused;
    Vector      data;                           /* Variable-length vector payload */
} HnswElementTupleData;
```

> [!NOTE]
> `HNSW_HEAPTIDS` is fixed at 10. When identical vector values are inserted, pgvector stores multiple heap TIDs inside the same graph node, preventing redundant nodes from fragmenting graph topology.

### Neighbor Tuple Format (`HnswNeighborTupleData`)
The outgoing edges for all layers of an element are stored separately from the vector data in an `HnswNeighborTupleData` struct:

```c
typedef struct HnswNeighborTupleData
{
    uint8       type;                           /* HNSW_NEIGHBOR_TUPLE_TYPE = 2 */
    uint8       version;                        /* Matches element tuple version */
    uint16      count;                          /* Total count of ItemPointers allocated */
    ItemPointerData indextids[FLEXIBLE_ARRAY_MEMBER];
} HnswNeighborTupleData;
```

### Cross-Page Storage & Stride Layout
A node's neighbor edges across all layers $l \in [0, \text{level}]$ are serialized into a single contiguous flat array of `ItemPointerData`:

```
Offset Stride Calculation:
Level 'level' (M items)      --> indextids[0                .. M - 1]
Level 'level - 1' (M items)  --> indextids[M                .. 2M - 1]
...
Level 1 (M items)            --> indextids[(level - 1) * M  .. level * M - 1]
Level 0 (2 * M items)        --> indextids[level * M        .. (level + 2) * M - 1]
```

Total number of edge pointers allocated is:

$$\text{count} = (\text{level} + 2) \cdot M$$

```c
#define HNSW_NEIGHBOR_TUPLE_SIZE(level, m) \
    MAXALIGN(add_size(offsetof(HnswNeighborTupleData, indextids), \
             mul_size(sizeof(ItemPointerData), mul_size(add_size(level, 2), m))))
```

```
PostgreSQL Page Layout (Block N):
┌──────────────────────────────────────────────────────────┐
│ PageHeaderData (pd_lsn, pd_checksum, pd_lower, pd_upper) │
├──────────────────────────────────────────────────────────┤
│ Linp 1: ItemIdData -> Offset to ElementTuple 1           │
│ Linp 2: ItemIdData -> Offset to NeighborTuple 1          │
│ Linp 3: ItemIdData -> Offset to ElementTuple 2           │
├──────────────────────────────────────────────────────────┤
│                     Unallocated Space                    │
├──────────────────────────────────────────────────────────┤
│ ElementTuple 2: [type=1|level=1|heaptids[10]|neighbortid]│
│ NeighborTuple 1: [type=2|count=(L+2)*M|indextids[...]]   │
│ ElementTuple 1: [type=1|level=2|heaptids[10]|neighbortid]│
├──────────────────────────────────────────────────────────┤
│ Special Space: HnswPageOpaqueData [nextblkno, 0xFF90]    │
└──────────────────────────────────────────────────────────┘
```

If both the element tuple and neighbor tuple fit on the current page (`combinedSize <= maxSize`), they are co-located on the same page. If their combined size exceeds free space, the neighbor tuple is allocated on a subsequent page (`neighborPage = blkno + 1, neighborOffno = FirstOffsetNumber`).

---

## 5. Planner Integration and Cost Estimation (`hnswcostestimate`)

### Costing Model Mathematical Derivation
PostgreSQL's cost planner executes `hnswcostestimate` in `hnsw.c`. When an index ordering clause is present (e.g. `ORDER BY val <-> '[1,2,3]'`), the planner calculates the ratio of index tuples that must be scanned to satisfy the query.

The cost formula balances the upper-layer descent against the layer-0 candidate beam expansion:

$$\begin{aligned}
\text{entryLevel} &= \lfloor \ln(N) \cdot m_L \rfloor = \lfloor \ln(N) / \ln(M) \rfloor \\
\text{layer0TuplesMax} &= M_0 \cdot ef\_search = 2M \cdot ef\_search \\
\text{layer0Selectivity} &= \alpha \cdot \frac{\ln(N)}{\ln(M) \cdot (1 + \ln(ef\_search))} \quad (\text{where } \alpha = 0.55) \\
\text{numIndexTuples} &= \min(N, \; \text{entryLevel} \cdot M + \text{layer0TuplesMax} \cdot \text{layer0Selectivity}) \\
\text{ratio} &= \frac{\text{numIndexTuples}}{N}
\end{aligned}$$

```c
/* In src/hnsw.c: hnswcostestimate */
double scalingFactor = 0.55;
int entryLevel = (int) (log(tuples) * HnswGetMl(m));
int layer0TuplesMax = HnswGetLayerM(m, 0) * hnsw_ef_search;
double layer0Selectivity = scalingFactor * log(tuples) / (log(m) * (1 + log(hnsw_ef_search)));

ratio = (entryLevel * m + layer0TuplesMax * layer0Selectivity) / tuples;
if (ratio > 1)
    ratio = 1;
```

### Handling Distance-Ordered Scans
1. **Startup Cost**: Represents the cost to reach layer 0 and retrieve the very first candidate vector:
   $$\text{indexStartupCost} = \text{indexTotalCost} \cdot \text{ratio}$$
2. **Missing Order Check**: If a query omits the `<->` ordering operator (e.g., standard equality `WHERE val = '[...]'`), HNSW sets cost to infinity (`get_float8_infinity()`), forcing the planner to reject the index.
3. **Sequential Page Cost Adjustment**: When index pages exceed table pages, startup costs are adjusted to prevent the planner from preferring sequential table scans over small index ranges.

---

## 6. GUC and Index Options Configuration

### Storage Parameters (Reloptions)
Reloptions are configured per-index inside the `WITH (...)` clause at index creation time:

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops) 
WITH (m = 16, ef_construction = 64);
```

| Parameter | Type | Default | Valid Range | Scope | Description |
|:---|:---|:---|:---|:---|:---|
| `m` | Integer | 16 | 2 .. 100 | Reloption | Maximum number of outgoing connections per node for upper layers ($M$). Ground layer automatically allocates $2M$. |
| `ef_construction` | Integer | 64 | 4 .. 1000 | Reloption | Size of dynamic nearest neighbor candidate pool used during index construction. Must satisfy $ef\_construction \ge 2M$. |

Validation enforces the $2M$ constraint in `InitBuildState` (`hnswbuild.c`):
```c
if (buildstate->efConstruction / 2 < buildstate->m)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("ef_construction must be greater than or equal to 2 * m")));
```

### Runtime GUC Variables
Session-level GUC parameters can be configured dynamically per connection or query:

```sql
SET hnsw.ef_search = 100;
SET hnsw.iterative_scan = 'relaxed_order';
```

| GUC Variable | Type | Default | Valid Range | Context | Description |
|:---|:---|:---|:---|:---|:---|
| `hnsw.ef_search` | Integer | 40 | 1 .. 1000 | `PGC_USERSET` | Size of dynamic candidate list during query execution. Higher values yield higher recall at the expense of query latency. |
| `hnsw.iterative_scan` | Enum | `off` | `off`, `relaxed_order`, `strict_order` | `PGC_USERSET` | Enables dynamic continuation scans beyond $ef\_search$ when filters restrict candidate rows. |
| `hnsw.max_scan_tuples` | Integer | 20000 | 1 .. `INT_MAX` | `PGC_USERSET` | Safety ceiling bounding the maximum number of graph nodes visited during iterative scans. |
| `hnsw.scan_mem_multiplier`| Float | 1.0 | 1.0 .. 1000.0 | `PGC_USERSET` | Multiplier applied to `work_mem` bounding iterative scan candidate heap allocations. |

---

## 7. Memory Management Architecture

### Memory Context Hierarchies
To prevent memory leaks across long-lived queries or large indexing sessions, `pgvector` allocates graph structures across explicit PostgreSQL `MemoryContext` boundaries:

```
TopMemoryContext / CurrentMemoryContext
 ├── Hnsw build graph context (GenerationContext, 1MB blocks)
 │    └── In-memory graph nodes, neighbor arrays, datum values
 ├── Hnsw build temporary context (AllocSet, default sizes)
 │    └── Per-tuple build callbacks, temporary calculations (reset per tuple)
 ├── Hnsw insert update context (GenerationContext, 128KB blocks)
 │    └── In-flight candidate loads during online insert
 ├── Hnsw scan temporary context (AllocSet, min 0, init 8KB, max 256KB)
 │    └── Query candidate heaps, visited hashes, unvisited buffers
 └── Hnsw vacuum temporary context (AllocSet)
      └── Repair element allocations and tid sets
```

> [!TIP]
> `graphCtx` uses a `GenerationContext` rather than a standard `AllocSet`. Because elements in HNSW are allocated monotonically during graph build and freed all at once upon flush, `GenerationContext` avoids internal free-list fragmentation and achieves higher memory density.

### The `HnswAllocator` Abstraction
`pgvector` decouples allocation logic from memory location using a function pointer wrapper:

```c
typedef struct HnswAllocator
{
    void       *(*alloc) (Size size, void *state);
    void       *state;
} HnswAllocator;
```

- In non-parallel builds: `alloc` points to `HnswMemoryContextAlloc`, allocating directly from backend-private `buildstate->graphCtx`.
- In parallel builds: `alloc` points to `HnswSharedMemoryAlloc`, carving contiguous chunks out of DSM `hnswarea` protected by `graph->allocatorLock`.

```c
static void *
HnswSharedMemoryAlloc(Size size, void *state)
{
    HnswBuildState *buildstate = (HnswBuildState *) state;
    Size alignedSize = MAXALIGN(size);
    Size newMemoryUsed = add_size(buildstate->graph->memoryUsed, alignedSize);

    if (newMemoryUsed > buildstate->graph->memoryTotal)
        elog(ERROR, "hnsw allocator out of memory");

    void *chunk = buildstate->hnswarea + buildstate->graph->memoryUsed;
    buildstate->graph->memoryUsed = newMemoryUsed;
    return chunk;
}
```

### Dynamic Shared Memory (DSM) Allocations
During parallel builds (`CREATE INDEX`), the leader estimates required shared memory:
1. `esthnswshared`: Size of `HnswShared` state + `ParallelTableScanDesc`.
2. `esthnswarea`: Size of graph storage, derived from `maintenance_work_mem * 1024 - 3MB` (with 3MB reserved for worker communication overhead and Docker shm margins), capped at `HNSW_MAX_GRAPH_MEMORY` (`SIZE_MAX / 2`).
3. Allocations never fail unexpectedly because a safety buffer (`memoryMargin = 1024 * 1024`) triggers a graceful flush to disk before DSM exhaustion.
