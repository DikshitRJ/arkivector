# PostgreSQL Extension Build System & CI/CD Architecture

A definitive, internals-level analysis of the build infrastructure, cross-platform compilation pipelines, testing harnesses, and release engineering in **pgvector (v0.8.7)**.

---

## Table of Contents
1. [The PGXS Build Framework](#1-the-pgxs-build-framework)
   - [What is PGXS?](#what-is-pgxs)
   - [The Role of pg_config](#the-role-of-pg_config)
   - [Core Makefile Structure & Standard Variables](#core-makefile-structure--standard-variables)
   - [Dynamic Rule Generation via PGXS Inclusion](#dynamic-rule-generation-via-pgxs-inclusion)
2. [Source Files & Compilation Pipeline](#2-source-files--compilation-pipeline)
   - [Exhaustive Source Inventory (The 19 Source Files)](#exhaustive-source-inventory-the-19-source-files)
   - [Compiler Flags & Optimization Strategy](#compiler-flags--optimization-strategy)
   - [Hardware Vectorization & Architecture Fallbacks](#hardware-vectorization--architecture-fallbacks)
   - [Runtime Dispatch via Target Clones](#runtime-dispatch-via-target-clones)
3. [Cross-Platform Build Architecture](#3-cross-platform-build-architecture)
   - [POSIX Make (`Makefile`) vs Windows NMAKE (`Makefile.win`)](#posix-make-makefile-vs-windows-nmake-makefilewin)
   - [MSVC Compilation Environment & Flags](#msvc-compilation-environment--flags)
   - [Windows Linking against postgres.lib](#windows-linking-against-postgreslib)
   - [Header Distribution for Downstream Extensions](#header-distribution-for-downstream-extensions)
4. [Installation Topology & Artifact Placement](#4-installation-topology--artifact-placement)
   - [Target Directory Layout](#target-directory-layout)
   - [Control File Installation](#control-file-installation)
   - [SQL Migration Script Deployment](#sql-migration-script-deployment)
   - [Uninstallation Semantics](#uninstallation-semantics)
5. [Testing Infrastructure & Quality Assurance](#5-testing-infrastructure--quality-assurance)
   - [The Dual-Engine Testing Methodology](#the-dual-engine-testing-methodology)
   - [SQL Regression Tests via pg_regress](#sql-regression-tests-via-pg_regress)
   - [Perl TAP Test Suite (PostgreSQL::Test::Cluster)](#perl-tap-test-suite-postgresqltestcluster)
   - [Physical Replication & WAL Verification Tests](#physical-replication--wal-verification-tests)
   - [Statistical Recall & ANN Accuracy Tests](#statistical-recall--ann-accuracy-tests)
   - [Edge Case Coverage: Storage Limits, Aggregates, and Float Parity](#edge-case-coverage-storage-limits-aggregates-and-float-parity)
6. [Containerization & Docker Packaging](#6-containerization--docker-packaging)
   - [Multi-Stage Production Dockerfile](#multi-stage-production-dockerfile)
   - [Minimizing Image Bloat & Layer Hygiene](#minimizing-image-bloat--layer-hygiene)
   - [Multi-Arch Builds (linux/amd64 and linux/arm64)](#multi-arch-builds-linuxamd64-and-linuxarm64)
   - [PGXN (PostgreSQL Extension Network) Packaging](#pgxn-postgresql-extension-network-packaging)
7. [Continuous Integration & Cross-Version Matrix](#7-continuous-integration--cross-version-matrix)
   - [GitHub Actions Matrix Breakdown](#github-actions-matrix-breakdown)
   - [Supported PostgreSQL Versions (PG 15 to PG 20)](#supported-postgresql-versions-pg-15-to-pg-20)
   - [Static Analysis via Clang scan-build](#static-analysis-via-clang-scan-build)
   - [Memory Sanitization & Undefined Behavior Auditing (Valgrind)](#memory-sanitization--undefined-behavior-auditing-valgrind)

---

## 1. The PGXS Build Framework

### What is PGXS?
**PGXS (PostgreSQL Extension Build Infrastructure)** is the standardized build framework provided by PostgreSQL to compile third-party extensions against an existing server installation without requiring the full PostgreSQL source tree.

Rather than authoring complex Autotools or CMake scripts, an extension author writes a concise `Makefile` declaring high-level metadata (such as extension name, object files, and migration scripts). PGXS supplies all compilation rules, link commands, header search paths, optimization flags, and installation targets.

```
+-------------------------------------------------------------+
|                      Extension Makefile                     |
|  Declares: EXTENSION, MODULE_big, OBJS, DATA, REGRESS, etc.  |
+-------------------------------------------------------------+
                              |
                              v
                  include $(shell pg_config --pgxs)
                              |
                              v
+-------------------------------------------------------------+
|                  PostgreSQL PGXS Subsystem                  |
|  $PGROOT/lib/pgxs/src/makefiles/pgxs.mk                     |
|  - Injects CFLAGS, CPPFLAGS, LDFLAGS from target Postgres   |
|  - Injects compile rules (%.o: %.c)                         |
|  - Defines targets: all, install, installcheck, clean       |
+-------------------------------------------------------------+
```

### The Role of pg_config
The centerpiece of PGXS is `pg_config`. Located in PostgreSQL's `bin/` directory, this executable emits the precise configuration parameters, directory paths, and compiler flags with which the target PostgreSQL server was built.

In `pgvector/Makefile`:

```makefile
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
```

By querying `pg_config`, PGXS retrieves:
- `--pgxs`: Location of `pgxs.mk` (e.g. `/usr/lib/postgresql/17/lib/pgxs/src/makefiles/pgxs.mk`).
- `--includedir-server`: Path to server internal headers (e.g. `/usr/include/postgresql/17/server`).
- `--pkglibdir`: Installation directory for dynamic libraries (`.so`).
- `--sharedir`: Installation directory for extension metadata and SQL scripts (`$SHAREDIR/extension`).
- `--cflags`: Core C compiler flags used to build PostgreSQL.
- `--ldflags`: Core linker flags.

### Core Makefile Structure & Standard Variables
In `pgvector/Makefile`, standard PGXS variables define the binary artifact:

```makefile
EXTENSION = vector
EXTVERSION = 0.8.7

MODULE_big = vector
DATA = $(wildcard sql/*--*--*.sql)
DATA_built = sql/$(EXTENSION)--$(EXTVERSION).sql
OBJS = src/bitutils.o src/bitvec.o src/halfutils.o src/halfvec.o src/hnsw.o \
       src/hnswbuild.o src/hnswinsert.o src/hnswscan.o src/hnswutils.o \
       src/hnswvacuum.o src/ivfbuild.o src/ivfflat.o src/ivfinsert.o \
       src/ivfkmeans.o src/ivfscan.o src/ivfutils.o src/ivfvacuum.o \
       src/sparsevec.o src/vector.o
HEADERS = src/halfvec.h src/sparsevec.h src/vector.h

TESTS = $(wildcard test/sql/*.sql)
REGRESS = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION)
```

#### Meaning of Each Variable:
1. `EXTENSION = vector`:
   Specifies the extension identifier. PGXS expects `<EXTENSION>.control` (`vector.control`) in the source root and deploys it to `$(SHAREDIR)/extension/`.
2. `EXTVERSION = 0.8.7`:
   The current release version string.
3. `MODULE_big = vector`:
   Directs PGXS to build a single shared object from multiple object files (`OBJS`). PGXS compiles each `.c` into `.o` and links them into `vector.so` (or `vector.dylib`). (If an extension only consists of a single file `vector.c`, the simpler variable `MODULES = vector` would be used instead).
4. `DATA = $(wildcard sql/*--*--*.sql)`:
   Declares static SQL migration scripts (e.g. `vector--0.8.6--0.8.7.sql`) to be copied directly into `$(SHAREDIR)/extension/`.
5. `DATA_built = sql/$(EXTENSION)--$(EXTVERSION).sql`:
   Declares SQL installation scripts that are dynamically created during the build step.
6. `OBJS`:
   The full list of compiled object files to be linked into the shared library.
7. `HEADERS`:
   C header files to be exported into the PostgreSQL server header tree for consumption by downstream extensions.
8. `REGRESS`:
   List of SQL regression test names (without `.sql` suffix) executed by `make installcheck`.
9. `REGRESS_OPTS`:
   Flags passed to `pg_regress`. `--inputdir=test` redirects input lookup to `test/sql/` and `test/expected/`. `--load-extension=$(EXTENSION)` instructs `pg_regress` to automatically execute `CREATE EXTENSION vector;` in the temporary test database prior to running test scripts.

### Dynamic Rule Generation via PGXS Inclusion
Notice lines 46–49 of `Makefile`:

```makefile
all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@
```

pgvector maintains its base SQL definition in `sql/vector.sql`. When `make` executes, this custom target copies `sql/vector.sql` to `sql/vector--0.8.7.sql`. This file is registered under `DATA_built`. When `make install` runs, PGXS copies both `DATA` and `DATA_built` into `$(SHAREDIR)/extension/`.

---

## 2. Source Files & Compilation Pipeline

### Exhaustive Source Inventory (The 19 Source Files)
pgvector's engine is partitioned into 19 C translation units (`src/*.c`) organized across four core functional areas:

```
pgvector/src/
├── Core Types & Distance Math
│   ├── vector.c        --> Float32 vector type, I/O, operators, distance metrics, aggregates
│   ├── halfvec.c       --> Float16 (halfvec) type, I/O, operators, distance metrics
│   ├── halfutils.c     --> FP16 conversion tables & software math
│   ├── bitvec.c        --> Binary vector (bit) type, Hamming & Jaccard distance
│   ├── bitutils.c      --> Popcount & bitwise optimization primitives
│   └── sparsevec.c     --> Compressed sparse vector type, I/O, sparse distance
│
├── HNSW Access Method
│   ├── hnsw.c          --> AM handler (hnswhandler), GUCs, options, cost estimation, validate
│   ├── hnswbuild.c     --> ambuild & ambuildempty, parallel build coordination (DSM)
│   ├── hnswinsert.c    --> aminsert, layer assignment, neighbor selection heuristics
│   ├── hnswscan.c      --> ambeginscan, amrescan, amgettuple, amendscan, iterative search
│   ├── hnswutils.c     --> Page format, element encoding, pairing heaps, hashing
│   └── hnswvacuum.c    --> ambulkdelete, amvacuumcleanup, 5-pass graph repair
│
└── IVFFlat Access Method
    ├── ivfflat.c       --> AM handler (ivfflathandler), GUCs, options, cost estimation, validate
    ├── ivfbuild.c      --> ambuild & ambuildempty, parallel worker coordination
    ├── ivfinsert.c     --> aminsert, centroid assignment, page splitting
    ├── ivfkmeans.c     --> K-means clustering algorithm, spherical k-means
    ├── ivfscan.c       --> ambeginscan, amrescan, amgettuple, multi-probe scan
    ├── ivfutils.c      --> Page utilities, list tracking, buffer commits
    └── ivfvacuum.c     --> ambulkdelete, amvacuumcleanup, list compaction
```

### Compiler Flags & Optimization Strategy
Vector distance calculations are floating-point intensive. In `pgvector/Makefile`:

```makefile
OPTFLAGS = -march=native

# Mac ARM doesn't always support -march=native
ifeq ($(shell uname -s), Darwin)
	ifeq ($(shell uname -p), arm)
		OPTFLAGS =
	endif
endif

# PowerPC doesn't support -march=native
ifneq ($(filter ppc64%, $(shell uname -m)), )
	OPTFLAGS =
endif

# RISC-V64 doesn't support -march=native
ifeq ($(shell uname -m), riscv64)
	OPTFLAGS =
endif

PG_CFLAGS += $(OPTFLAGS) -ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -ffp-contract=fast
```

#### Detailed Breakdown of Compiler Directives:
1. `-march=native`:
   Enables code generation targeting all instruction set extensions supported by the local compiling host CPU (including AVX2, AVX-512, FMA on x86_64, or NEON on ARM).
2. `-ftree-vectorize`:
   Enables loop auto-vectorization passes in GCC and Clang, transforming standard C loops (such as iterating over dimensions to compute sum of squared differences) into SIMD instructions without inline assembly.
3. `-fassociative-math`:
   Allows the compiler to re-associate floating-point arithmetic expressions (e.g. `(a + b) + c -> a + (b + c)`). IEEE 754 floating-point addition is non-associative due to rounding; this flag permits SIMD vector reduction trees.
4. `-fno-signed-zeros`:
   Ignores the sign of zero (+0.0 vs -0.0), allowing optimizations that treat `x + 0.0` as `x`.
5. `-fno-trapping-math`:
   Assumes floating-point operations cannot generate hardware traps (exceptions), enabling aggressive instruction reordering and pipelining.
6. `-ffp-contract=fast`:
   Enables floating-point expression contraction, allowing the compiler to generate fused multiply-add (`FMA`) instructions (`a * b + c` in a single clock cycle with single rounding).

### Hardware Vectorization & Architecture Fallbacks
Certain CPU architectures or platform compilers do not support `-march=native`:
- **macOS Apple Silicon (ARM64)**: Clang on macOS does not recognize `-march=native` identically to GCC and may emit compilation warnings or errors. pgvector checks `uname -s == Darwin` and `uname -p == arm` and clears `OPTFLAGS`.
- **PowerPC (`ppc64`, `ppc64le`)**: Clears `OPTFLAGS`.
- **RISC-V 64 (`riscv64`)**: Clears `OPTFLAGS`.
- **Generic / Distributable Binaries**: Package maintainers building precompiled binaries (e.g. Debian, Homebrew, Alpine) can override flags by invoking `make OPTFLAGS=""`.

### Runtime Dispatch via Target Clones
In `src/vector.c`:

```c
#if defined(USE_TARGET_CLONES) && !defined(__FMA__)
#define VECTOR_TARGET_CLONES __attribute__((target_clones("default", "fma")))
#else
#define VECTOR_TARGET_CLONES
#endif
```

On compilers supporting GCC's `target_clones` attribute (when FMA was not already enabled globally via `-march`), functions tagged with `VECTOR_TARGET_CLONES` compile multiple function versions into the binary. At runtime, the GNU indirect function (IFUNC) mechanism selects the fastest SIMD path based on CPUID feature detection.

---

## 3. Cross-Platform Build Architecture

### POSIX Make (`Makefile`) vs Windows NMAKE (`Makefile.win`)
PostgreSQL natively supports Windows using Microsoft Visual C++ (MSVC). However, Windows does not provide GNU Make or POSIX shell utilities by default.

pgvector provides a standalone, dedicated Windows build file: `Makefile.win`.
- On Linux, macOS, and BSD: Developers build with `make` and `make install` using `Makefile`.
- On Windows: Developers build using Microsoft's `nmake` utility and Visual Studio compiler toolchains (`cl.exe`, `link.exe`).

### MSVC Compilation Environment & Flags
In `pgvector/Makefile.win`:

```makefile
!ifndef PGROOT
!error PGROOT is not set
!endif
BINDIR = $(PGROOT)\bin
INCLUDEDIR = $(PGROOT)\include
INCLUDEDIR_SERVER = $(PGROOT)\include\server
LIBDIR = $(PGROOT)\lib
PKGLIBDIR = $(PGROOT)\lib
SHAREDIR = $(PGROOT)\share

OPTFLAGS =
PG_CFLAGS = $(PG_CFLAGS) $(OPTFLAGS) /O2 /fp:fast

!if exists("$(INCLUDEDIR_SERVER)\optimizer\extendplan.h")
PG_CFLAGS = $(PG_CFLAGS) /std:c11
!endif

CFLAGS = /nologo /I"$(INCLUDEDIR_SERVER)\port\win32_msvc" /I"$(INCLUDEDIR_SERVER)\port\win32" /I"$(INCLUDEDIR_SERVER)" /I"$(INCLUDEDIR)" $(PG_CFLAGS)
```

#### MSVC Optimization Flags:
- `/O2`: Maximum speed optimization (equivalent to `-O3` in GCC).
- `/fp:fast`: Enables fast floating-point mode in MSVC. Relaxes IEEE 754 precision constraints to allow SIMD auto-vectorization and FMA generation.
- `/std:c11`: Applied on PostgreSQL 19+ headers where modern C11 features are required.

### Windows Linking against `postgres.lib`
On Unix/Linux, shared libraries (`.so`) can leave symbols unresolved at link time; the dynamic loader (`ld.so`) resolves them when the library is loaded into the PostgreSQL server process.

On Windows, the Portable Executable (PE/COFF) loader **strictly forbids unresolved symbols in DLLs**. A dynamic link library (`.dll`) cannot be created unless an import library resolving all exported PostgreSQL functions is supplied at link time.

In `pgvector/Makefile.win`:

```makefile
SHLIB = $(EXTENSION).dll
LIBS = "$(LIBDIR)\postgres.lib"

$(SHLIB): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LIBS) /link /DLL /OUT:$(SHLIB)
```

`postgres.lib` is the import library distributed with PostgreSQL for Windows. It provides the import table entries linking `vector.dll` calls (`palloc`, `ReadBuffer`, `elog`, etc.) directly into `postgres.exe`.

### Header Distribution for Downstream Extensions
Other PostgreSQL extensions may want to declare functions taking vectors as arguments or interact with `Vector` structures directly in C.

pgvector exports its header files into PostgreSQL's internal include directory during installation:
- On POSIX (`Makefile`):
  PGXS automatically installs `HEADERS = src/halfvec.h src/sparsevec.h src/vector.h` into `$(INCLUDEDIR_SERVER)/extension/vector/`.
- On Windows (`Makefile.win`):
  ```makefile
  if not exist "$(INCLUDEDIR_SERVER)\extension\$(EXTENSION)" mkdir "$(INCLUDEDIR_SERVER)\extension\$(EXTENSION)"
  for %f in ($(HEADERS)) do copy %f "$(INCLUDEDIR_SERVER)\extension\$(EXTENSION)"
  ```

---

## 4. Installation Topology & Artifact Placement

### Target Directory Layout
When `make install` is executed, PGXS distributes the compiled artifacts into the PostgreSQL directory hierarchy:

```
$PGROOT/ (Base PostgreSQL Installation)
│
├── lib/ (or lib/x86_64-linux-gnu/)
│   └── vector.so                           <-- Shared library binary
│
├── share/extension/
│   ├── vector.control                      <-- Extension manifest
│   ├── vector--0.8.7.sql                   <-- Primary installation DDL
│   ├── vector--0.8.6--0.8.7.sql            <-- Version migration DDLs
│   ├── vector--0.8.5--0.8.6.sql
│   └── ... (all historical upgrade scripts)
│
└── include/server/extension/vector/
    ├── vector.h                            <-- C development header
    ├── halfvec.h                           <-- C development header
    └── sparsevec.h                         <-- C development header
```

### Control File Installation
`vector.control` is copied to `$(SHAREDIR)/extension/vector.control`.
PostgreSQL reads this file whenever a user connects to a database and runs `SELECT * FROM pg_available_extensions;` or executes `CREATE EXTENSION vector;`.

### SQL Migration Script Deployment
PGXS evaluates:
```makefile
DATA = $(wildcard sql/*--*--*.sql)
DATA_built = sql/$(EXTENSION)--$(EXTVERSION).sql
```
Every file matching `sql/*--*--*.sql` and the generated `sql/vector--0.8.7.sql` are copied to `$(SHAREDIR)/extension/`.
This ensures that whether a user installs pgvector fresh (`CREATE EXTENSION vector;`) or upgrades an older installation (`ALTER EXTENSION vector UPDATE;`), PostgreSQL finds the required SQL script on disk.

### Uninstallation Semantics
In `Makefile.win`:
```makefile
uninstall:
	del /f "$(PKGLIBDIR)\$(SHLIB)"
	del /f "$(SHAREDIR)\extension\$(EXTENSION).control"
	del /f "$(SHAREDIR)\extension\$(EXTENSION)--*.sql"
	del /f "$(INCLUDEDIR_SERVER)\extension\$(EXTENSION)\*.h"
	rmdir "$(INCLUDEDIR_SERVER)\extension\$(EXTENSION)"
```
POSIX PGXS provides a built-in `uninstall` rule that removes the corresponding files from `PKGLIBDIR`, `SHAREDIR`, and `INCLUDEDIR_SERVER`.

---

## 5. Testing Infrastructure & Quality Assurance

### The Dual-Engine Testing Methodology
pgvector employs a comprehensive, dual-engine testing architecture:
1. **Engine 1: SQL Regression Tests (`pg_regress`)**: Fast, deterministic functional test suite verifying SQL operators, syntax, error conditions, and basic indexing.
2. **Engine 2: Perl TAP Tests (`PostgreSQL::Test::Cluster`)**: Full orchestration test suite that spins up real PostgreSQL clusters, configures streaming replication, verifies crash recovery/WAL replay, and measures statistical recall of ANN searches.

```
+-----------------------------------------------------------------------------------------+
|                                pgvector Test Pipeline                                   |
+-----------------------------------------------------------------------------------------+
| 1. make installcheck (SQL Regression)    | 2. make prove_installcheck (Perl TAP Tests)  |
|    - Driven by pg_regress                |    - Driven by Test::More / IPC::Run         |
|    - 14 test suites in test/sql/         |    - 48 end-to-end integration test files    |
|    - Output matched against test/expected|    - Spawns multi-node primary/replica setup |
|    - Produces regression.diffs on error  |    - Tests WAL recovery, recall, aggregates  |
+-----------------------------------------------------------------------------------------+
```

### SQL Regression Tests via `pg_regress`
When `make installcheck` runs, PGXS invokes `pg_regress`:

```makefile
TESTS = $(wildcard test/sql/*.sql)
REGRESS = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION)
```

The test files in `test/sql/`:
- `vector_type.sql`: Basic dimension validation, NaN rejection, text format parsing.
- `halfvec.sql`, `sparsevec.sql`, `bit.sql`: Type validation for alternative formats.
- `btree.sql`: Verifies B-tree scalar ordering opclass (`vector_ops`).
- `copy.sql`: Verifies binary and text `COPY` commands.
- `cast.sql`: Verifies explicit and assignment casts between vector types.
- `hnsw_vector.sql`, `hnsw_halfvec.sql`, `hnsw_sparsevec.sql`, `hnsw_bit.sql`: HNSW indexing syntax and operations.
- `ivfflat_vector.sql`, `ivfflat_halfvec.sql`, `ivfflat_bit.sql`: IVFFlat indexing syntax and operations.

#### The Diff-Matching Mechanism
For each test (e.g. `test/sql/hnsw_vector.sql`):
1. `pg_regress` connects to a test database and runs the SQL script.
2. It captures `stdout` and `stderr` to `results/hnsw_vector.out`.
3. It compares `results/hnsw_vector.out` against `test/expected/hnsw_vector.out`.
4. If output differs, it records the diff in `regression.diffs` and exits with an error code.

### Perl TAP Test Suite (`PostgreSQL::Test::Cluster`)
Modern PostgreSQL provides a Perl-based Test Anything Protocol (TAP) testing framework. In `pgvector/Makefile`:

```makefile
prove_installcheck:
	rm -rf $(CURDIR)/tmp_check
	cd $(srcdir) && TESTDIR='$(CURDIR)' PATH="$(bindir):$$PATH" PGPORT='6$(DEF_PGPORT)' PG_REGRESS='$(top_builddir)/src/test/regress/pg_regress' $(PROVE) $(PG_PROVE_FLAGS) $(PROVE_FLAGS) $(if $(PROVE_TESTS),$(PROVE_TESTS),test/t/*.pl)
```

Running `make prove_installcheck` executes all 48 test scripts in `test/t/*.pl` using `prove`.

### Physical Replication & WAL Verification Tests
Generic WAL safety cannot be tested within a single database connection. `test/t/001_ivfflat_wal.pl` and `test/t/010_hnsw_wal.pl` verify that index changes stream across physical WAL to standby replicas.

In `test/t/001_ivfflat_wal.pl`:
```perl
# 1. Initialize and start Primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

# 2. Take physical basebackup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# 3. Initialize streaming replica from backup
my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->init_from_backup($node_primary, $backup_name, has_streaming => 1);
$node_replica->start;

# 4. Perform insertions & index builds on Primary
$node_primary->safe_psql("postgres", "CREATE EXTENSION vector;");
$node_primary->safe_psql("postgres", "CREATE TABLE tst (v vector(32));");
$node_primary->safe_psql("postgres", "CREATE INDEX ON tst USING ivfflat (v);");

# 5. Wait for replica to catch up to primary LSN
my $caughtup_query = "SELECT pg_current_wal_lsn() <= replay_lsn FROM pg_stat_replication WHERE application_name = '$applname';";
$node_primary->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for replica 1 to catch up";

# 6. Execute index scan queries on BOTH nodes and assert bit-for-bit identical results
my $primary_result = $node_primary->safe_psql("postgres", $queries);
my $replica_result = $node_replica->safe_psql("postgres", $queries);
is($primary_result, $replica_result, "$test_name: query result matches");
```

This verifies that:
- `GenericXLog` records are valid and replayable by PostgreSQL's `generic_redo` engine.
- Standby replicas reconstruct an identical HNSW/IVFFlat index in memory and on disk.

### Statistical Recall & ANN Accuracy Tests
Approximate Nearest Neighbor (ANN) indexes trade 100% exact recall for speed. But how can automated tests verify an index works correctly without false negatives?

In `test/t/012_hnsw_vector_build_recall.pl`:
1. Seeds 10,000 random 3-dimensional vectors.
2. Runs 20 queries with `enable_seqscan = on` to obtain **exact ground truth results** via sequential scan.
3. Builds an HNSW index (`CREATE INDEX idx ON tst USING hnsw (v vector_l2_ops)`).
4. Sets `enable_seqscan = off` to force `Index Scan`.
5. Compares index results against ground truth:
   ```perl
   my $min = $operator eq "<#>" ? 0.97 : 0.99;
   cmp_ok($correct / $total, ">=", $min, $operator);
   ```
6. Runs tests across:
   - Serial build (`max_parallel_maintenance_workers = 0`)
   - Parallel in-memory build
   - Parallel on-disk build (`maintenance_work_mem = '4MB'`)

### Edge Case Coverage: Storage Limits, Aggregates, and Float Parity
- **`019_storage.pl` (TOAST & Page Capacity)**:
  Inserts rows containing three 1024-dimension vectors. Verifies that inserts succeed under default `STORAGE = external`, but fail with `row is too big` when altered to `STORAGE PLAIN`.
- **`018_aggregates.pl` (Parallel Aggregates)**:
  Generates 1,000,000 vectors. Verifies that `avg(v)` and `sum(v)` produce results consistent with PostgreSQL's native `real` type and verifies parallel aggregation (`Partial Aggregate` in EXPLAIN).
- **`033_comparison.pl` (Float Parity)**:
  Compares `vector_cmp()`, `halfvec_cmp()`, and `sparsevec_cmp()` against PostgreSQL's native `btarraycmp()`, guaranteeing that vector sorting behaves identically to PostgreSQL's native array sorting.

---

## 6. Containerization & Docker Packaging

### Multi-Stage Production Dockerfile
The pgvector repository includes a production-grade `Dockerfile` designed to produce minimal image layers:

```dockerfile
# syntax=docker/dockerfile:1

ARG PG_MAJOR=17
ARG DEBIAN_CODENAME=bookworm
FROM postgres:$PG_MAJOR-$DEBIAN_CODENAME
ARG PG_MAJOR

ADD https://github.com/pgvector/pgvector.git#v0.8.7 /tmp/pgvector

RUN apt-get update && \
		apt-mark hold locales && \
		apt-get install -y --no-install-recommends build-essential postgresql-server-dev-$PG_MAJOR && \
		cd /tmp/pgvector && \
		make clean && \
		make OPTFLAGS="" && \
		make install && \
		mkdir /usr/share/doc/pgvector && \
		cp LICENSE README.md /usr/share/doc/pgvector && \
		rm -r /tmp/pgvector && \
		apt-get remove -y build-essential postgresql-server-dev-$PG_MAJOR && \
		apt-get autoremove -y && \
		apt-mark unhold locales && \
		rm -rf /var/lib/apt/lists/*
```

### Minimizing Image Bloat & Layer Hygiene
1. **Single `RUN` Command**: The entire installation and cleanup execute within a single Docker layer. If `apt-get remove` were run in a separate subsequent `RUN` step, the intermediate compilers and headers would remain frozen in the lower image layers.
2. **`apt-mark hold locales`**: Prevents `apt-get autoremove` from inadvertently purging locale packages required by PostgreSQL.
3. **`make OPTFLAGS=""`**: Crucial for portable container images! When compiling inside a Docker container, compiling with `-march=native` would bake host-specific instructions (e.g. AVX-512) into the container image. Running that image on an older x86_64 host would crash with `SIGILL` (Illegal Instruction). Disabling `OPTFLAGS` ensures binary portability across diverse deployment hosts.
4. **Toolchain Purge**: Uninstalls `build-essential` and `postgresql-server-dev-$PG_MAJOR` and purges `/var/lib/apt/lists/*`, keeping the final Docker image footprint minimal.

### Multi-Arch Builds (`linux/amd64` and `linux/arm64`)
In `pgvector/Makefile`, targets for Docker release leverage Docker Buildx:

```makefile
docker-release:
	docker buildx build --push --pull --no-cache \
	  --platform linux/amd64,linux/arm64 \
	  --build-arg PG_MAJOR=$(PG_MAJOR) \
	  --build-arg DEBIAN_CODENAME=bookworm \
	  -t pgvector/pgvector:pg$(PG_MAJOR) \
	  -t pgvector/pgvector:$(EXTVERSION)-pg$(PG_MAJOR) .
```

This cross-compiles native container images for both standard x86 servers (`linux/amd64`) and ARM-based servers (`linux/arm64`, AWS Graviton, Apple Silicon).

### PGXN (PostgreSQL Extension Network) Packaging
pgvector includes a `META.json` conforming to the PGXN Meta Spec (v1.0.0):
- Declares extension name, version (`0.8.7`), license, maintainer.
- Declares prerequisites: runtime dependency on PostgreSQL $\ge 15.0.0$.
- Allows users to install pgvector via `pgxn install vector`.

---

## 7. Continuous Integration & Cross-Version Matrix

### GitHub Actions Matrix Breakdown
pgvector's continuous integration (`.github/workflows/build.yml`) implements an exhaustive test matrix covering 5 distinct operating system environments and 6 major PostgreSQL versions.

```
+-----------------------------------------------------------------------------------------+
|                                CI Test Matrix Architecture                              |
+-----------------------------------------------------------------------------------------+
| Job Target  | Architecture | OS Platform         | PostgreSQL Versions Tested           |
+-------------+--------------+---------------------+--------------------------------------+
| ubuntu      | x86_64 / arm | Ubuntu 22.04 - 26.04| PG 15, PG 16, PG 17, PG 18, PG 19, 20|
| mac         | ARM / Intel  | macOS 15, macOS 26  | PG 15, PG 18                         |
| windows     | x86_64 (MSVC)| Windows Server 2025 | PG 17                                |
| i386        | 32-bit x86   | Debian 12 (i386)    | PG 15                                |
| valgrind    | x86_64       | Ubuntu Linux        | PG 18 (Memory audit + UB check)      |
+-----------------------------------------------------------------------------------------+
```

### Supported PostgreSQL Versions (PG 15 to PG 20)
Notice the extensive version compatibility handled dynamically in the code:
- **PG 15**: Baseline supported version.
- **PG 16**: Modernizes `varatt.h` inclusion and introduces `amsummarizing = false`.
- **PG 17**: Adapts `parser/scansup.h` scanner macros and introduces `amcanbuildparallel` / `aminsertcleanup`.
- **PG 18**: Implements `PG_MODULE_MAGIC_EXT`, `get_opfamily_name`, `vacuum_delay_point(false)`, and `disabled_nodes`.
- **PG 19 & 20**: Adopts `Item` as `Pointer`, integrates `storage/read_stream.h`, uses `palloc_array()`, and supports static `IndexAmRoutine` initialization.

### Strict Compilation Flags in CI
In `.github/workflows/build.yml`:

```yaml
- run: make
  env:
    PG_CFLAGS: -DUSE_ASSERT_CHECKING -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare
```

- `-DUSE_ASSERT_CHECKING`: Enables PostgreSQL's internal `Assert()` macros. Any invalid pointer access, corrupted heap TID, or incorrect layer level triggers an immediate crash during test runs.
- `-Wall -Wextra -Werror`: Promotes all compiler warnings to fatal compilation errors, guaranteeing pristine code quality.

### Static Analysis via Clang `scan-build`
In the macOS CI workflow:

```yaml
- run: make clean && $(brew --prefix llvm@$LLVM_VERSION)/bin/scan-build --status-bugs make
  env:
    PG_CFLAGS: -DUSE_ASSERT_CHECKING
```

Clang's `scan-build` static analyzer executes path-sensitive interprocedural analysis across the C source files, intercepting potential null pointer dereferences, uninitialized variable reads, and resource leaks.

### Memory Sanitization & Undefined Behavior Auditing (Valgrind)
In `.github/workflows/build.yml`:

```yaml
valgrind:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v7
    - uses: ankane/setup-postgres-valgrind@v1
      with:
        postgres-version: 18
        check-ub: yes
    - run: make OPTFLAGS=""
    - run: sudo --preserve-env=PG_CONFIG make install
    - run: make installcheck
```

Runs the entire test suite under **Valgrind Memcheck**:
- Detects uninitialized memory access in vector math buffers.
- Verifies that `palloc` pointers do not escape their assigned `MemoryContext`.
- `check-ub: yes`: Enables Clang/GCC Undefined Behavior Sanitizer (UBSan) to trap integer overflows, unaligned memory dereferences, and float division anomalies.

---

## Build System Reference Summary

```
                      +-----------------------------+
                      |         Developer           |
                      |   make OPTFLAGS=""          |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |          Compiler           |
                      |   GCC / Clang / MSVC        |
                      |   -ftree-vectorize          |
                      |   -ffp-contract=fast        |
                      +-----------------------------+
                                     |
                                     v
                      +-----------------------------+
                      |       Shared Object         |
                      |   vector.so / vector.dll    |
                      +-----------------------------+
                                     |
            +------------------------+------------------------+
            |                                                 |
            v                                                 v
+-----------------------+                         +-----------------------+
|  make installcheck    |                         | make prove_installcheck
|  - pg_regress         |                         | - Test::Cluster       |
|  - 14 SQL test suites |                         | - Replication & WAL   |
|  - Expected output    |                         | - Statistical recall  |
+-----------------------+                         +-----------------------+
```
