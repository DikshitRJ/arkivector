# pgvector-python: SQLAlchemy & SQLModel Integration Deep Dive

> **Document Version:** 1.0.0  
> **Target SDK:** `pgvector-python` (`pgvector.sqlalchemy`)  
> **Framework Compatibility:** SQLAlchemy 2.0+ (Sync & Async), SQLModel (Pydantic v2 + SQLAlchemy)  
> **Supported DBAPI Drivers:** `psycopg` (v3), `psycopg2`, `asyncpg`, `pg8000`  
> **Source Files Inspected:**  
> - `pgvector/sqlalchemy/__init__.py`  
> - `pgvector/sqlalchemy/vector.py`  
> - `pgvector/sqlalchemy/halfvec.py`  
> - `pgvector/sqlalchemy/sparsevec.py`  
> - `pgvector/sqlalchemy/bit.py`  
> - `pgvector/sqlalchemy/functions.py`  
> - `tests/test_sqlalchemy.py`  
> - `tests/test_sqlmodel.py`  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architectural Foundation](#2-architectural-foundation)
   - [2.1 SQLAlchemy 2.0 Type Engine Overview](#21-sqlalchemy-20-type-engine-overview)
   - [2.2 DBAPI Driver Agnosticism](#22-dbapi-driver-agnosticism)
   - [2.3 Type Hierarchy Architecture](#23-type-hierarchy-architecture)
3. [Column Type Implementations](#3-column-type-implementations)
   - [3.1 VECTOR (UserDefinedType)](#31-vector-userdefinedtype)
   - [3.2 HALFVEC (UserDefinedType)](#32-halfvec-userdefinedtype)
   - [3.3 SPARSEVEC (UserDefinedType)](#33-sparsevec-userdefinedtype)
   - [3.4 BIT (TypeDecorator over PGBit)](#34-bit-typedecorator-over-pgbit)
   - [3.5 Reflection Hook via `ischema_names`](#35-reflection-hook-via-ischema_names)
4. [Serialization, Deserialization & Caching Mechanics](#4-serialization-deserialization--caching-mechanics)
   - [4.1 Statement Cache Compliance (`cache_ok = True`)](#41-statement-cache-compliance-cache_ok--true)
   - [4.2 Inbound Bind Processor (`bind_processor`)](#42-inbound-bind-processor-bind_processor)
   - [4.3 Literal Value Compilation (`literal_processor`)](#43-literal-value-compilation-literal_processor)
   - [4.4 Outbound Result Processor (`result_processor`)](#44-outbound-result-processor-result_processor)
5. [Operator System & Distance Expressions](#5-operator-system--distance-expressions)
   - [5.1 Comparator Architecture](#51-comparator-architecture)
   - [5.2 Vector Distance Operators (`<->`, `<#>`, `<=>`, `<+>`)](#52-vector-distance-operators-----)
   - [5.3 Bit Distance Operators (`<~>`, `<%>`)](#53-bit-distance-operators--)
   - [5.4 Aggregate Functions (`avg`, `sum`)](#54-aggregate-functions-avg-sum)
6. [Index Definitions (HNSW & IVFFlat)](#6-index-definitions-hnsw--ivfflat)
   - [6.1 Table DDL and Core Index Specifications](#61-table-ddl-and-core-index-specifications)
   - [6.2 Operator Classes (`vector_l2_ops`, `vector_cosine_ops`, etc.)](#62-operator-classes-vector_l2_ops-vector_cosine_ops-etc)
   - [6.3 Expression Indexes (Half-Precision & Binary Quantization)](#63-expression-indexes-half-precision--binary-quantization)
7. [Query Patterns & Real-World Usage](#7-query-patterns--real-world-usage)
   - [7.1 Core vs ORM Nearest-Neighbor Query Patterns](#71-core-vs-orm-nearest-neighbor-query-patterns)
   - [7.2 Distance Threshold Filtering](#72-distance-threshold-filtering)
   - [7.3 Subqueries & Scalar Subquery Distance Computations](#73-subqueries--scalar-subquery-distance-computations)
   - [7.4 Binary Quantization Pre-Filtering with Full-Precision Re-Ranking](#74-binary-quantization-pre-filtering-with-full-precision-re-ranking)
   - [7.5 Aggregate Operations](#75-aggregate-operations)
8. [Multi-Driver & Asynchronous Execution](#8-multi-driver--asynchronous-execution)
   - [8.1 Synchronous Drivers (`psycopg`, `psycopg2`, `pg8000`)](#81-synchronous-drivers-psycopg-psycopg2-pg8000)
   - [8.2 Asynchronous Engines (`asyncpg`, `psycopg` async)](#82-asynchronous-engines-asyncpg-psycopg-async)
   - [8.3 Array Handling (`ARRAY(VECTOR(n))`) Across Drivers](#83-array-handling-arrayvectorn-across-drivers)
9. [SQLModel Integration (FastAPI Ecosystem)](#9-sqlmodel-integration-fastapi-ecosystem)
   - [9.1 Schema Declaration via `sa_type`](#91-schema-declaration-via-sa_type)
   - [9.2 The `col()` Expression Wrapper Necessity](#92-the-col-expression-wrapper-necessity)
   - [9.3 Arbitrary Types Configuration](#93-arbitrary-types-configuration)
   - [9.4 Known Upstream Limitations & Workarounds](#94-known-upstream-limitations--workarounds)
10. [Architectural Diagrams](#10-architectural-diagrams)

---

## 1. Executive Summary

The `pgvector.sqlalchemy` package exposes first-class support for PostgreSQL's `vector` extension within SQLAlchemy 2.0+ (both Core and ORM) as well as SQLModel. Rather than forcing developers to execute raw SQL or un-typed string interpolations, `pgvector-python` defines native column types (`VECTOR`, `HALFVEC`, `SPARSEVEC`, and `BIT`) that hook directly into SQLAlchemy's compilation, type-coercion, and operator systems.

Key architectural highlights include:
- **Clean Subclassing:** `VECTOR`, `HALFVEC`, and `SPARSEVEC` extend `UserDefinedType[Any]`, while `BIT` extends `TypeDecorator[Any]` over the PostgreSQL dialect's native `PGBit` type.
- **SQLAlchemy 2.0 Statement Cache Compatibility:** All types set `cache_ok = True` or `inherit_cache = True`, guaranteeing compatibility with SQLAlchemy's high-performance compiled statement cache.
- **Custom Comparator Factories:** Column attributes directly expose distance calculation methods (`l2_distance`, `max_inner_product`, `cosine_distance`, `l1_distance`, `hamming_distance`, `jaccard_distance`) returning standard SQLAlchemy `BinaryExpression` operators that compile into native PostgreSQL operators (`<->`, `<#>`, `<=>`, `<+>`, `<~>`, `<%>`).
- **Catalog Reflection Support:** Automatically injects types into SQLAlchemy's PostgreSQL dialect type map (`ischema_names['vector'] = VECTOR`), enabling reflection of existing database tables via `Table(..., autoload_with=engine)` and `automap_base()`.
- **Seamless SQLModel Compatibility:** Integrates with SQLModel models by specifying `sa_type=VECTOR(dim)` within SQLModel's `Field()`, bridging Pydantic data validation with PostgreSQL vector storage.

---

## 2. Architectural Foundation

### 2.1 SQLAlchemy 2.0 Type Engine Overview

SQLAlchemy divides type management into two layers:
1. **DDL Generation & Schema Inspection:** Handled by `get_col_spec()` and database reflection dictionaries (`ischema_names`).
2. **Runtime Value Processing:** Handled via `bind_processor()` (Python value to DB parameter), `literal_processor()` (Python value rendered into an inline SQL literal), and `result_processor()` (raw DB return value to Python object).

`pgvector-python` implements this contract cleanly:

```
+--------------------------------------------------------------------+
|                         SQLAlchemy Engine                          |
+--------------------------------------------------------------------+
                                  |
            +---------------------+---------------------+
            |                                           |
            v                                           v
+------------------------+                  +------------------------+
|      DDL Pipeline      |                  |     DML / Query Flow   |
+------------------------+                  +------------------------+
| get_col_spec()         |                  | bind_processor()       |
| -> "VECTOR(1536)"      |                  | literal_processor()    |
| ischema_names hook     |                  | result_processor()     |
+------------------------+                  | Comparator factory     |
                                            +------------------------+
```

### 2.2 DBAPI Driver Agnosticism

SQLAlchemy abstracts DBAPI drivers behind its `Dialect` interface. The underlying driver for PostgreSQL might be:
- `psycopg` (version 3)
- `psycopg2`
- `asyncpg`
- `pg8000`

`pgvector.sqlalchemy` ensures vector types operate reliably regardless of whether the underlying driver has executed low-level type registration (e.g. `pgvector.psycopg.register_vector(conn)`). If the driver cannot natively adapt a Python list into a vector OID, the `bind_processor` converts the vector to a canonical string format `"[1.0,2.0,3.0]"` which PostgreSQL casts implicitly upon receipt.

### 2.3 Type Hierarchy Architecture

```mermaid
classDiagram
    class TypeEngine~T~ {
        +get_col_spec()
        +bind_processor()
        +result_processor()
    }

    class UserDefinedType~T~ {
        +cache_ok: bool
    }

    class TypeDecorator~T~ {
        +impl
        +process_bind_param()
        +process_result_value()
    }

    class VECTOR {
        +dim: int | None
        +get_col_spec()
        +bind_processor()
        +literal_processor()
        +result_processor()
        +comparator_factory: VECTOR.Comparator
    }

    class HALFVEC {
        +dim: int | None
        +get_col_spec()
        +bind_processor()
        +literal_processor()
        +result_processor()
        +comparator_factory: HALFVEC.Comparator
    }

    class SPARSEVEC {
        +dim: int | None
        +get_col_spec()
        +bind_processor()
        +literal_processor()
        +result_processor()
        +comparator_factory: SPARSEVEC.Comparator
    }

    class BIT {
        +impl: PGBit
        +process_bind_param()
        +comparator_factory: BIT.Comparator
    }

    TypeEngine <|-- UserDefinedType
    TypeEngine <|-- TypeDecorator
    UserDefinedType <|-- VECTOR
    UserDefinedType <|-- HALFVEC
    UserDefinedType <|-- SPARSEVEC
    TypeDecorator <|-- BIT
```

---

## 3. Column Type Implementations

### 3.1 VECTOR (UserDefinedType)

Defined in `pgvector/sqlalchemy/vector.py`:

```python
class VECTOR(UserDefinedType[Any]):
    cache_ok = True
    _string = String()

    def __init__(self, dim: int | None = None) -> None:
        super().__init__()
        self.dim = dim

    def get_col_spec(self, **kw: Any) -> str:
        if self.dim is None:
            return 'VECTOR'
        return 'VECTOR(%d)' % self.dim
```

- **Dimension Flexibility:** `dim` is optional. If `dim=None`, it emits unconstrained `VECTOR`, which permits vectors of varying dimensions per row (though unindexed). If `dim=1536`, it emits `VECTOR(1536)`.
- **String Delegate:** Retains an internal `_string = String()` instance specifically to borrow its `_cached_literal_processor` when rendering literal binds.

### 3.2 HALFVEC (UserDefinedType)

Defined in `pgvector/sqlalchemy/halfvec.py`:

```python
class HALFVEC(UserDefinedType[Any]):
    cache_ok = True
    _string = String()

    def __init__(self, dim: int | None = None) -> None:
        super().__init__()
        self.dim = dim

    def get_col_spec(self, **kw: Any) -> str:
        if self.dim is None:
            return 'HALFVEC'
        return 'HALFVEC(%d)' % self.dim
```

- Generates `HALFVEC` or `HALFVEC(dim)` column specifications.
- Translates to 16-bit half-precision floating-point storage (`halfvec` in PostgreSQL 0.7.0+), cutting disk and RAM usage by 50% compared to standard single-precision `VECTOR`.

### 3.3 SPARSEVEC (UserDefinedType)

Defined in `pgvector/sqlalchemy/sparsevec.py`:

```python
class SPARSEVEC(UserDefinedType[Any]):
    cache_ok = True
    _string = String()

    def __init__(self, dim: int | None = None) -> None:
        super().__init__()
        self.dim = dim

    def get_col_spec(self, **kw: Any) -> str:
        if self.dim is None:
            return 'SPARSEVEC'
        return 'SPARSEVEC(%d)' % self.dim
```

- Generates `SPARSEVEC(dim)` specifications.
- Specifically deserializes database values into instances of `pgvector.SparseVector` (preserving compressed sparse representation) rather than raw float lists.

### 3.4 BIT (TypeDecorator over PGBit)

Defined in `pgvector/sqlalchemy/bit.py`:

```python
class BIT(TypeDecorator[Any]):
    impl = PGBit
    cache_ok = True

    def process_bind_param(self, value: Any, dialect: Any) -> Any:
        if dialect.__class__.__name__ == 'PGDialect_asyncpg' and isinstance(value, str):
            import asyncpg
            return asyncpg.BitString(value)  # type: ignore
        return value
```

Unlike dense and sparse vectors, `BIT` uses SQLAlchemy's `TypeDecorator`, wrapping PostgreSQL's standard dialect type `PGBit` (`sqlalchemy.dialects.postgresql.base.PGBit`).
- **Driver Workaround:** `asyncpg` has unique typing rules for PostgreSQL bit strings; passing a Python `str` to a `bit(n)` column through `asyncpg` throws a protocol-level type error. The `process_bind_param` method inspects the dialect name and automatically wraps strings into `asyncpg.BitString(value)` when communicating with `asyncpg`.

### 3.5 Reflection Hook via `ischema_names`

At the bottom of `vector.py`, `halfvec.py`, and `sparsevec.py`, each module registers itself directly into SQLAlchemy's dialect registry:

```python
from sqlalchemy.dialects.postgresql.base import ischema_names

ischema_names['vector'] = VECTOR
ischema_names['halfvec'] = HALFVEC
ischema_names['sparsevec'] = SPARSEVEC
```

This allows SQLAlchemy table reflection and `automap` to seamlessly detect vector columns:

```python
# Automatic reflection of existing schema
metadata = MetaData()
metadata.reflect(engine, only=['items'])

# The type is automatically mapped to VECTOR
assert isinstance(metadata.tables['items'].c.embedding.type, VECTOR)
```

---

## 4. Serialization, Deserialization & Caching Mechanics

### 4.1 Statement Cache Compliance (`cache_ok = True`)

SQLAlchemy 2.0 incorporates a statement execution cache to avoid recompiling SQL ASTs on every query. If any type in a query lacks a `cache_ok = True` class attribute, SQLAlchemy disables the statement cache for that query and emits a warning.

Every type in `pgvector.sqlalchemy` explicitly declares:
```python
cache_ok = True
```
This guarantees zero performance penalty in high-throughput query loops.

### 4.2 Inbound Bind Processor (`bind_processor`)

When a query contains a bind parameter (e.g. `session.add(Item(embedding=[1.0, 2.0, 3.0]))`), SQLAlchemy calls `bind_processor(dialect)`:

```python
def bind_processor(self, dialect: Dialect) -> Any:
    def process(value: Any) -> str | None:
        return Vector._to_db(value)
    return process
```

Under the hood, `Vector._to_db` handles multiple input types:
- `None` $\rightarrow$ `None`
- `list[float]` $\rightarrow$ Formats directly to `"[1.0,2.0,3.0]"`
- `numpy.ndarray` $\rightarrow$ Validates `ndim == 1`, verifies numeric dtype, casts to float32, and outputs string format
- `Vector` instance $\rightarrow$ Calls `value.to_text()`

### 4.3 Literal Value Compilation (`literal_processor`)

When compiling statements with `compile_kwargs={'literal_binds': True}` (used in debugging, raw SQL export, or offline migrations):

```python
def literal_processor(self, dialect: Dialect) -> Any:
    string_literal_processor = self._string._cached_literal_processor(dialect)

    def process(value: Any) -> Any:
        return string_literal_processor(Vector._to_db(value))
    return process
```

This ensures proper SQL literal quoting, rendering valid statements like:
```sql
SELECT items.id FROM items ORDER BY items.embedding <-> '[1.0,2.0,3.0]' LIMIT 5
```

### 4.4 Outbound Result Processor (`result_processor`)

When PostgreSQL returns column data across the wire:

```python
def result_processor(self, dialect: Dialect, coltype: Any) -> Any:
    def process(value: Any) -> list[float] | None:
        return Vector._from_db(value)
    return process
```

- If the DBAPI driver already converted the data into a `list` or `Vector` object (because `register_vector()` was run on the connection), `Vector._from_db` unwraps it.
- If the driver returned raw string text (e.g. `"[1,2,3]"`), `Vector._from_db` parses it via `Vector._from_text()` into Python floats.
- For `SPARSEVEC`, the result processor yields a native `SparseVector` instance.

---

## 5. Operator System & Distance Expressions

### 5.1 Comparator Architecture

In SQLAlchemy, operator overloading for custom types is configured via a nested `Comparator` class inheriting from `TypeEngine.Comparator` or `TypeDecorator.Comparator`:

```python
class VECTOR(UserDefinedType[Any]):
    class Comparator(TypeEngine.Comparator[Any]):
        def l2_distance(self, other: object, /) -> Operators:
            return self.op('<->', return_type=Float)(other)
        ...

    comparator_factory = Comparator
```

Calling `Item.embedding.l2_distance([1, 2, 3])` invokes `self.op('<->', return_type=Float)(...)`, which constructs a `BinaryExpression` where:
- Left-hand side: The table column `Item.embedding`
- Operator: `<->`
- Right-hand side: The query vector (bound parameter)
- Return Type: `Float` (enabling arithmetic comparisons like `< 1.0`)

### 5.2 Vector Distance Operators (`<->`, `<#>`, `<=>`, `<+>`)

The three vector types (`VECTOR`, `HALFVEC`, `SPARSEVEC`) implement identical comparator interfaces:

| Python Method | SQL Operator | Distance Metric | Range | Return Type |
|---|---|---|---|---|
| `col.l2_distance(vec)` | `<->` | Euclidean / L2 Distance | $[0, \infty)$ | `Float` |
| `col.max_inner_product(vec)` | `<#>` | Negative Inner Product | $(-\infty, \infty)$ | `Float` |
| `col.cosine_distance(vec)` | `<=>` | Cosine Distance ($1 - \cos(\theta)$) | $[0, 2]$ | `Float` |
| `col.l1_distance(vec)` | `<+>` | Taxicab / Manhattan (L1) | $[0, \infty)$ | `Float` |

> **Note on Inner Product:** PostgreSQL vector index operators sort ascending. To support nearest-neighbor retrieval using inner product, the `<#>` operator computes the *negative* inner product. Therefore, minimizing `<#>` maximizes inner product.

### 5.3 Bit Distance Operators (`<~>`, `<%>`)

The `BIT` comparator supports binary string distance metrics:

| Python Method | SQL Operator | Distance Metric | Description |
|---|---|---|---|
| `col.hamming_distance(bits)` | `<~>` | Hamming Distance | Number of bit positions that differ |
| `col.jaccard_distance(bits)` | `<%>` | Jaccard Distance | Disagreement over union of set bits |

### 5.4 Aggregate Functions (`avg`, `sum`)

Defined in `pgvector/sqlalchemy/functions.py`:

```python
from sqlalchemy.sql.functions import ReturnTypeFromArgs, sum
from typing import Any

class avg(ReturnTypeFromArgs[Any]):
    inherit_cache = True
    package = 'pgvector'

__all__ = ['avg', 'sum']
```

- **`avg` Function:** pgvector provides a custom PostgreSQL aggregate `AVG(vector)` that computes the centroid (element-wise average) of a vector set. The Python `avg` class extends `ReturnTypeFromArgs[Any]`, ensuring the return type matches the vector column type rather than defaulting to `Float`.
- **`sum` Import:** Re-exported directly from `sqlalchemy.sql.functions` to maintain a unified import path for vector aggregations.

```python
from pgvector.sqlalchemy import avg, sum

# Computes centroid embedding of all matching items
centroid = session.scalars(select(avg(Item.embedding))).first()
# Returns: [2.5, 3.5, 4.5]
```

---

## 6. Index Definitions (HNSW & IVFFlat)

### 6.1 Table DDL and Core Index Specifications

SQLAlchemy creates vector indexes using its standard `Index` construct, configuring PostgreSQL-specific options through `postgresql_using`, `postgresql_with`, and `postgresql_ops`.

```python
from sqlalchemy import Index

# HNSW Index on standard vector
hnsw_index = Index(
    'my_hnsw_idx',
    Item.embedding,
    postgresql_using='hnsw',
    postgresql_with={'m': 16, 'ef_construction': 64},
    postgresql_ops={'embedding': 'vector_l2_ops'}
)

# IVFFlat Index
ivfflat_index = Index(
    'my_ivfflat_idx',
    Item.embedding,
    postgresql_using='ivfflat',
    postgresql_with={'lists': 100},
    postgresql_ops={'embedding': 'vector_l2_ops'}
)
```

Generated SQL DDL:
```sql
CREATE INDEX my_hnsw_idx ON items USING hnsw (embedding vector_l2_ops) WITH (m = 16, ef_construction = 64);
CREATE INDEX my_ivfflat_idx ON items USING ivfflat (embedding vector_l2_ops) WITH (lists = 100);
```

### 6.2 Operator Classes (`vector_l2_ops`, `vector_cosine_ops`, etc.)

The `postgresql_ops` dictionary specifies the PostgreSQL operator class matching the search metric:

| Vector Type | Metric | Operator Class |
|---|---|---|
| `VECTOR` | L2 / Euclidean | `vector_l2_ops` |
| `VECTOR` | Cosine Distance | `vector_cosine_ops` |
| `VECTOR` | Inner Product | `vector_ip_ops` |
| `VECTOR` | L1 / Manhattan | `vector_l1_ops` |
| `HALFVEC` | L2 / Euclidean | `halfvec_l2_ops` |
| `HALFVEC` | Cosine Distance | `halfvec_cosine_ops` |
| `HALFVEC` | Inner Product | `halfvec_ip_ops` |
| `HALFVEC` | L1 / Manhattan | `halfvec_l1_ops` |
| `SPARSEVEC` | L2 / Euclidean | `sparsevec_l2_ops` |
| `SPARSEVEC` | Cosine Distance | `sparsevec_cosine_ops` |
| `SPARSEVEC` | Inner Product | `sparsevec_ip_ops` |
| `SPARSEVEC` | L1 / Manhattan | `sparsevec_l1_ops` |
| `BIT` | Hamming Distance | `bit_hamming_ops` |
| `BIT` | Jaccard Distance | `bit_jaccard_ops` |

### 6.3 Expression Indexes (Half-Precision & Binary Quantization)

SQLAlchemy allows building indexes on casted or transformed expressions using `func.cast()` or `func.binary_quantize()`.

#### Half-Precision Index on Full-Precision Column
Saves 50% index memory while maintaining full-precision column data:

```python
from sqlalchemy.sql import func
from pgvector.sqlalchemy import HALFVEC

half_precision_index = Index(
    'items_half_precision_idx',
    func.cast(Item.embedding, HALFVEC(1536)).label('embedding'),
    postgresql_using='hnsw',
    postgresql_ops={'embedding': 'halfvec_l2_ops'}
)
```

#### Binary Quantization Index
Compresses float vectors to 1-bit binary representations for ultra-fast candidate filtering:

```python
from pgvector.sqlalchemy import BIT

binary_quantize_index = Index(
    'items_binary_quantize_idx',
    func.cast(func.binary_quantize(Item.embedding), BIT(1536)).label('embedding'),
    postgresql_using='hnsw',
    postgresql_ops={'embedding': 'bit_hamming_ops'}
)
```

---

## 7. Query Patterns & Real-World Usage

### 7.1 Core vs ORM Nearest-Neighbor Query Patterns

#### 1. SQLAlchemy ORM 2.0 Style (`session.scalars`)
```python
query_vector = [1.0, 2.0, 3.0]

stmt = (
    select(Item)
    .order_by(Item.embedding.cosine_distance(query_vector))
    .limit(5)
)
results = session.scalars(stmt).all()
```

#### 2. Projecting Distance Score alongside Entities
```python
distance_col = Item.embedding.cosine_distance(query_vector).label('distance')

stmt = (
    select(Item, distance_col)
    .order_by(distance_col)
    .limit(5)
)
for item, distance in session.execute(stmt):
    print(f"Item {item.id}, distance: {distance:.4f}")
```

#### 3. Legacy ORM 1.4 Style (`session.query`)
```python
items = (
    session.query(Item)
    .order_by(Item.embedding.l2_distance(query_vector))
    .limit(5)
    .all()
)
```

### 7.2 Distance Threshold Filtering

Because the distance comparator sets `return_type=Float`, comparison operators (`<`, `<=`, `>`, `>=`) evaluate as valid SQL filters:

```python
# Select all items within L2 distance < 0.8
stmt = (
    select(Item)
    .filter(Item.embedding.l2_distance([1.0, 1.0, 1.0]) < 0.8)
    .order_by(Item.embedding.l2_distance([1.0, 1.0, 1.0]))
)
nearby_items = session.scalars(stmt).all()
```

### 7.3 Subqueries & Scalar Subquery Distance Computations

Search for items similar to a specific database record using scalar subqueries:

```python
# Find items similar to item with id = 42
target_vector = (
    select(Item.embedding)
    .filter_by(id=42)
    .scalar_subquery()
)

stmt = (
    select(Item)
    .filter(Item.id != 42)
    .order_by(Item.embedding.cosine_distance(target_vector))
    .limit(5)
)
similar_items = session.scalars(stmt).all()
```

### 7.4 Binary Quantization Pre-Filtering with Full-Precision Re-Ranking

A high-performance pattern combining Hamming pre-filtering on a subquery with full-precision cosine re-ranking:

```python
query_vec = [0.5, -1.2, 3.1]
dim = 3

# Step 1: Binary quantize query vector
quantized_query = func.binary_quantize(func.cast(literal(query_vec, VECTOR), VECTOR(dim)))

# Step 2: Hamming distance expression
hamming_dist = func.cast(
    func.binary_quantize(Item.embedding), BIT(dim)
).hamming_distance(quantized_query)

# Step 3: Top 20 candidate subquery
candidates_subq = (
    session.query(Item)
    .order_by(hamming_dist)
    .limit(20)
    .subquery()
)

# Step 4: Re-rank top 20 candidates by exact cosine distance
final_results = (
    session.query(candidates_subq)
    .order_by(candidates_subq.c.embedding.cosine_distance(query_vec))
    .limit(5)
    .all()
)
```

### 7.5 Aggregate Operations

```python
from pgvector.sqlalchemy import avg, sum

# Compute centroid of cluster
cluster_avg = session.scalars(
    select(avg(Item.embedding)).filter(Item.category == 'electronics')
).one()

# Vector summation
cluster_sum = session.scalars(
    select(sum(Item.embedding)).filter(Item.category == 'electronics')
).one()
```

---

## 8. Multi-Driver & Asynchronous Execution

### 8.1 Synchronous Drivers (`psycopg`, `psycopg2`, `pg8000`)

To leverage driver-level native binary vector decoding rather than text fallback, register the adapter on engine connection events:

```python
from sqlalchemy import create_engine, event

# psycopg (v3)
engine_psycopg = create_engine('postgresql+psycopg://user:pass@localhost/db')
@event.listens_for(engine_psycopg, 'connect')
def connect_psycopg(dbapi_connection, connection_record):
    from pgvector.psycopg import register_vector
    register_vector(dbapi_connection)

# psycopg2
engine_psycopg2 = create_engine('postgresql+psycopg2://user:pass@localhost/db')
@event.listens_for(engine_psycopg2, 'connect')
def connect_psycopg2(dbapi_connection, connection_record):
    from pgvector.psycopg2 import register_vector
    register_vector(dbapi_connection)
```

### 8.2 Asynchronous Engines (`asyncpg`, `psycopg` async)

Asynchronous execution requires registering vectors asynchronously:

```python
from sqlalchemy.ext.asyncio import create_async_engine, async_sessionmaker

# asyncpg
async_engine = create_async_engine('postgresql+asyncpg://user:pass@localhost/db')

@event.listens_for(async_engine.sync_engine, 'connect')
def connect_asyncpg(dbapi_connection, connection_record):
    from pgvector.asyncpg import register_vector
    dbapi_connection.run_async(register_vector)

# Query execution
async_session = async_sessionmaker(async_engine, expire_on_commit=False)
async with async_session() as session:
    stmt = select(Item).order_by(Item.embedding.l2_distance([1.0, 2.0, 3.0])).limit(5)
    result = await session.scalars(stmt)
    items = result.all()
```

### 8.3 Array Handling (`ARRAY(VECTOR(n))`) Across Drivers

SQLAlchemy supports arrays of vectors, which is critical for multi-vector architectures (e.g., ColBERT token embeddings):

```python
from sqlalchemy import ARRAY
from pgvector.sqlalchemy import VECTOR

class Document(Base):
    __tablename__ = 'documents'
    id: Mapped[int] = mapped_column(primary_key=True)
    token_embeddings: Mapped[list[list[float]]] = mapped_column(ARRAY(VECTOR(128)))
```

> [!WARNING] Driver Casting Limitation
> Drivers like `asyncpg` and `psycopg` without low-level type registration will format nested Python lists into `text[]` rather than `vector[]`, causing PostgreSQL type mismatch errors. Always register vector types via connection events when working with `ARRAY(VECTOR)`.

---

## 9. SQLModel Integration (FastAPI Ecosystem)

SQLModel merges Pydantic v2 validation with SQLAlchemy ORM models. Because SQLModel does not introduce custom vector fields, it relies directly on `pgvector.sqlalchemy` via SQLAlchemy column override mechanisms.

### 9.1 Schema Declaration via `sa_type`

Pass `sa_type=VECTOR(dim)` to `sqlmodel.Field()`:

```python
from sqlmodel import SQLModel, Field, create_engine, select, col
from pgvector.sqlalchemy import VECTOR, HALFVEC, BIT, SPARSEVEC
from pgvector import SparseVector

class Item(SQLModel, table=True):
    __tablename__ = 'sqlmodel_items'

    id: int | None = Field(default=None, primary_key=True)
    embedding: list[float] | None = Field(default=None, sa_type=VECTOR(1536))
    half_embedding: list[float] | None = Field(default=None, sa_type=HALFVEC(1536))
    binary_embedding: str | None = Field(default=None, sa_type=BIT(1536))
    sparse_embedding: SparseVector | None = Field(default=None, sa_type=SPARSEVEC(1536))
```

### 9.2 The `col()` Expression Wrapper Necessity

In SQLModel, model attributes represent both Pydantic model fields and SQLAlchemy column descriptors. When constructing distance expressions or indexes, standard static analysis and typing require wrapping the attribute in SQLModel's `col()` function:

```python
# Creating an HNSW index with SQLModel
from sqlmodel import Index

index = Index(
    'sqlmodel_items_hnsw_idx',
    col(Item.embedding),
    postgresql_using='hnsw',
    postgresql_with={'m': 16, 'ef_construction': 64},
    postgresql_ops={'embedding': 'vector_l2_ops'}
)

# Executing a vector similarity query
stmt = (
    select(Item)
    .order_by(col(Item.embedding).l2_distance([1.0, 2.0, 3.0]))
    .limit(5)
)
results = session.exec(stmt).all()
```

Without `col()`, linters and type checkers report that `list[float]` has no attribute `l2_distance`.

### 9.3 Arbitrary Types Configuration

When using types like `pgvector.SparseVector` that are not native Pydantic types, enable `arbitrary_types_allowed`:

```python
from pydantic import ConfigDict

class Item(SQLModel, table=True):
    model_config = ConfigDict(arbitrary_types_allowed=True)
    ...
    sparse_embedding: SparseVector | None = Field(default=None, sa_type=SPARSEVEC(30522))
```

### 9.4 Known Upstream Limitations & Workarounds

| Issue | Upstream Reference | Root Cause | Workaround |
|---|---|---|---|
| `session.exec(text(...))` type error | `fastapi/sqlmodel#909`, `#1657` | Type stub mismatch for raw SQL execution in SQLModel session | Add `# type: ignore` or use `session.connection().execute(...)` |
| `Field(sa_type=...)` typing warning | `fastapi/sqlmodel#1228`, `#1345` | Pydantic type validator expects known SQLAlchemy type instances | Add `# type: ignore` on field definition |

---

## 10. Architectural Diagrams

### 10.1 Query Execution Flow

```mermaid
sequenceDiagram
    autonumber
    actor App as Application / Service
    participant ORM as SQLAlchemy ORM / SQLModel
    participant Comp as VECTOR.Comparator
    participant Dialect as PGDialect / Compiler
    participant DB as PostgreSQL (pgvector)

    App->>ORM: select(Item).order_by(Item.embedding.cosine_distance([1,2,3]))
    ORM->>Comp: cosine_distance([1,2,3])
    Comp->>Comp: self.op('<=>', return_type=Float)([1,2,3])
    Comp-->>ORM: BinaryExpression (embedding <=> :param_1)
    ORM->>Dialect: compile(statement)
    Dialect->>Dialect: bind_processor(VECTOR) -> Vector._to_db([1,2,3]) -> '[1.0,2.0,3.0]'
    Dialect->>DB: SELECT id, embedding FROM items ORDER BY embedding <=> $1 LIMIT 5
    DB-->>Dialect: Wire result: id=1, embedding='[1.0,2.0,3.0]'
    Dialect->>Dialect: result_processor(VECTOR) -> Vector._from_db('[1.0,2.0,3.0]')
    Dialect-->>ORM: Item(id=1, embedding=[1.0, 2.0, 3.0])
    ORM-->>App: List[Item]
```
