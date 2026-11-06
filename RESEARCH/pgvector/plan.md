# pgvector Research Plan

> Branch: `research/pgvector`  
> Goal: Complete, deeply technical documentation of pgvector (C extension) + pgvector-python (SDK)  
> Strategy: Parallel subagents, each targeting specific file groups → modular commits per phase

---

## Proposed Output Directory Structure

```
RESEARCH/pgvector/
├── plan.md                          ← This file
├── README.md                        ← Master overview & navigation
├── WHAT_WE_LEARN.md                 ← Meta-lessons: how such systems are built
│
├── architecture/
│   ├── overview.md                  ← High-level system architecture
│   ├── postgres_extension_model.md  ← How Postgres extensions work (hooks, catalog, etc.)
│   ├── data_flow.md                 ← End-to-end data flow: insert → index → query
│   └── build_system.md             ← Makefile, compilation, .control, META.json
│
├── core/
│   ├── vector_type.md               ← vector.c / vector.h: type system deep dive
│   ├── halfvec_type.md              ← halfvec.c / halfvec.h: half-precision float vectors
│   ├── bitvec_type.md               ← bitvec.c / bitvec.h: binary vectors
│   ├── sparsevec_type.md            ← sparsevec.c / sparsevec.h: sparse vectors
│   ├── distance_functions.md        ← All distance metrics: L2, cosine, inner product, etc.
│   └── type_casting.md              ← Casting between vector types
│
├── indexes/
│   ├── hnsw_overview.md             ← HNSW algorithm theory + pgvector impl
│   ├── hnsw_build.md                ← hnswbuild.c: index construction
│   ├── hnsw_insert.md               ← hnswinsert.c: incremental inserts
│   ├── hnsw_scan.md                 ← hnswscan.c: ANN query execution
│   ├── hnsw_vacuum.md               ← hnswvacuum.c: dead tuple cleanup
│   ├── hnsw_utils.md                ← hnswutils.c: shared utilities
│   ├── ivfflat_overview.md          ← IVFFlat algorithm theory + pgvector impl
│   ├── ivfflat_build.md             ← ivfbuild.c: index construction + k-means
│   ├── ivfflat_insert.md            ← ivfinsert.c: incremental inserts
│   ├── ivfflat_scan.md              ← ivfscan.c: ANN query execution
│   ├── ivfflat_vacuum.md            ← ivfvacuum.c: dead tuple cleanup
│   ├── ivfflat_kmeans.md            ← ivfkmeans.c: centroid computation
│   └── index_comparison.md          ← HNSW vs IVFFlat tradeoffs
│
├── sql-layer/
│   ├── sql_definitions.md           ← vector.sql: all SQL types, functions, operators
│   ├── migration_history.md         ← Version-by-version evolution (0.1.0 → 0.9.0)
│   ├── operators.md                 ← All operators and their implementations
│   └── aggregates.md                ← avg(), sum() aggregate functions
│
├── python-sdk/
│   ├── overview.md                  ← Package structure, what it provides
│   ├── core_types.md                ← vector.py, halfvec.py, sparsevec.py, bit.py
│   ├── psycopg2_adapter.md          ← psycopg2/ integration
│   ├── psycopg_adapter.md           ← psycopg (v3)/ integration
│   ├── asyncpg_adapter.md           ← asyncpg/ integration
│   ├── pg8000_adapter.md            ← pg8000/ integration
│   ├── sqlalchemy_integration.md    ← sqlalchemy/ types, functions
│   ├── django_integration.md        ← django/ fields, indexes, functions
│   ├── peewee_integration.md        ← peewee/ integration
│   └── examples_analysis.md         ← Analysis of all examples/
│
├── node-sdk/
│   ├── overview.md                  ← Package structure, ESM, TypeScript, ecosystem
│   ├── core_types.md                ← fromSql, toSql, SparseVector, utils
│   ├── pg_driver.md                 ← node-postgres (pg) integration
│   ├── postgres_driver.md           ← postgres.js tagged template integration
│   ├── pg_promise_driver.md         ← pg-promise & slonik integration
│   ├── knex_integration.md          ← Knex query builder & schema extensions
│   ├── kysely_integration.md        ← Kysely type-safe SQL query builder
│   ├── objection_integration.md     ← Objection.js model lifecycle & relations
│   ├── sequelize_integration.md     ← Sequelize custom DataTypes & models
│   ├── mikro_orm_integration.md     ← MikroORM custom Type<T> implementations
│   ├── prisma_drizzle_typeorm.md    ← Prisma, Drizzle ORM, TypeORM patterns
│   └── examples_analysis.md         ← Analysis of all 12 Node.js examples
│
└── lessons/
    └── WHAT_WE_LEARN.md             ← (alias for ../WHAT_WE_LEARN.md or symlink)
```

---

## Phases & Subagents

### Phase 1 — Core C Types (Parallel: 4 subagents)

| Subagent | Files Targeted | Output |
|----------|---------------|--------|
| `type-vector` | `src/vector.c`, `src/vector.h`, `sql/vector.sql` | `core/vector_type.md` |
| `type-halfvec` | `src/halfvec.c`, `src/halfvec.h`, `src/halfutils.c/h` | `core/halfvec_type.md` |
| `type-bitvec-sparse` | `src/bitvec.c/h`, `src/bitutils.c/h`, `src/sparsevec.c/h` | `core/bitvec_type.md`, `core/sparsevec_type.md` |
| `sql-layer` | `sql/vector.sql`, all migration sqls | `sql-layer/sql_definitions.md`, `sql-layer/migration_history.md`, `sql-layer/operators.md` |

**Commit:** `research: Phase 1 - core types documentation`

---

### Phase 2 — Index Engines (Parallel: 2 subagents)

| Subagent | Files Targeted | Output |
|----------|---------------|--------|
| `hnsw-researcher` | `src/hnsw.c/h`, `src/hnswbuild.c`, `src/hnswinsert.c`, `src/hnswscan.c`, `src/hnswutils.c`, `src/hnswvacuum.c` | `indexes/hnsw_*.md` |
| `ivf-researcher` | `src/ivfflat.c/h`, `src/ivfbuild.c`, `src/ivfinsert.c`, `src/ivfscan.c`, `src/ivfkmeans.c`, `src/ivfutils.c`, `src/ivfvacuum.c` | `indexes/ivfflat_*.md` |

**Commit:** `research: Phase 2 - index engines documentation`

---

### Phase 3 — Python SDK (Parallel: 3 subagents)

| Subagent | Files Targeted | Output |
|----------|---------------|--------|
| `sdk-core` | `pgvector/vector.py`, `halfvec.py`, `sparsevec.py`, `bit.py`, `_utils.py`, `__init__.py` | `python-sdk/core_types.md`, `python-sdk/overview.md` |
| `sdk-db-adapters` | `pgvector/psycopg2/`, `pgvector/psycopg/`, `pgvector/asyncpg/`, `pgvector/pg8000/` | `python-sdk/psycopg2_adapter.md`, `python-sdk/psycopg_adapter.md`, etc. |
| `sdk-orm` | `pgvector/sqlalchemy/`, `pgvector/django/`, `pgvector/peewee/` + all `examples/` | `python-sdk/sqlalchemy_integration.md`, `python-sdk/django_integration.md`, `python-sdk/examples_analysis.md` |

**Commit:** `research: Phase 3 - Python SDK documentation`

---

### Phase 4 — Architecture & Cross-Cutting (Parallel: 2 subagents)

| Subagent | Files Targeted | Output |
|----------|---------------|--------|
| `arch-researcher` | All `src/` headers, `vector.control`, `Makefile`, `META.json`, `Dockerfile`, CI workflows | `architecture/overview.md`, `architecture/postgres_extension_model.md`, `architecture/build_system.md` |
| `index-compare` | All test files (`test/t/*.pl`), CHANGELOG.md | `indexes/index_comparison.md`, `architecture/data_flow.md`, `sql-layer/aggregates.md` |

**Commit:** `research: Phase 4 - architecture and cross-cutting documentation`

---

### Phase 5 — Synthesis & Master Docs (1 subagent)

| Subagent | Input | Output |
|----------|-------|--------|
| `synthesizer` | All Phase 1-4 outputs | `README.md`, `WHAT_WE_LEARN.md`, `core/distance_functions.md`, `core/type_casting.md`, `indexes/index_comparison.md` (final), `sql-layer/migration_history.md` (enriched) |

**Commit:** `research: Phase 5 - synthesis and master documentation`

---

### Phase 6 — Node.js & TypeScript SDK (Parallel: 5 subagents)

| Subagent | Files Targeted | Output |
|----------|---------------|--------|
| `node-sdk-core` | `package.json`, `src/index.js`, `src/utils.js`, `src/sparse-vector.js`, `types/` | `node-sdk/overview.md`, `node-sdk/core_types.md` |
| `node-sdk-drivers` | `src/pg/`, `src/postgres/`, `src/pg-promise/`, `tests/` | `node-sdk/pg_driver.md`, `node-sdk/postgres_driver.md`, `node-sdk/pg_promise_driver.md` |
| `node-sdk-query-builders` | `src/knex/`, `src/kysely/`, `src/objection/`, `types/` | `node-sdk/knex_integration.md`, `node-sdk/kysely_integration.md`, `node-sdk/objection_integration.md` |
| `node-sdk-orms` | `src/sequelize/`, `src/mikro-orm/`, `prisma/`, `tests/` | `node-sdk/sequelize_integration.md`, `node-sdk/mikro_orm_integration.md`, `node-sdk/prisma_drizzle_typeorm.md` |
| `node-sdk-examples` | All 12 examples in `examples/` | `node-sdk/examples_analysis.md` |

**Commits:** Modular commits per specialist focus on `research/pgvector`

---

## Key Questions Each Phase Must Answer

### Phase 1 (Types)
- How does pgvector register a custom type with Postgres?
- What is the binary/text representation format of each type?
- How are the input/output functions (`vector_in`, `vector_out`) implemented?
- What arithmetic operations are supported at the C level?
- How does half-precision (fp16) work in memory vs fp32?

### Phase 2 (Indexes)
- How does HNSW graph construction work in Postgres pages?
- How does IVFFlat k-means clustering happen at build time?
- How does Postgres WAL (Write-Ahead Log) interact with both indexes?
- What is the page layout of HNSW and IVFFlat on disk?
- How does `ivf_probes` / `hnsw_ef_search` affect scan quality?
- How does vacuum clean dead tuples from these custom indexes?

### Phase 3 (Python SDK)
- How does each DB adapter register pgvector types?
- How does psycopg2 vs psycopg3 differ in adaptation approach?
- How does SQLAlchemy define custom column types that map to pgvector?
- How do Django ORM fields integrate with pgvector?
- What patterns do the examples demonstrate?

### Phase 4 (Architecture)
- What Postgres extension APIs are used? (`PG_MODULE_MAGIC`, `PG_FUNCTION_INFO_V1`, am callbacks)
- How does the access method (AM) API work for custom index types?
- How does pgvector handle concurrent reads/writes?

### Phase 5 (Synthesis)
- What is the full end-to-end data flow of a vector similarity search?
- What are the key design decisions and tradeoffs?
- What can we learn about building Postgres extensions from scratch?
