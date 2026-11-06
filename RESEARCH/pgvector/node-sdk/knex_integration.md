# pgvector-node: Knex.js Integration Deep Dive

> **Document Version:** 1.0.0  
> **Target SDK:** `pgvector-node` (`pgvector/knex`)  
> **Framework Compatibility:** Knex.js 2.x, 3.x+  
> **Supported PostgreSQL Driver:** `pg` (node-postgres)  
> **Source Files Inspected:**  
> - `src/knex/index.js`  
> - `src/knex/index.d.ts`  
> - `types/knex/index.d.ts`  
> - `src/utils.js`  
> - `src/index.js`  
> - `tests/knex.test.js`  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architectural Mechanics & Extension Model](#2-architectural-mechanics--extension-model)
   - [2.1 Knex Prototype Extension Architecture](#21-knex-prototype-extension-architecture)
   - [2.2 Evaluation Lifecycle & Side-Effect Registration](#22-evaluation-lifecycle--side-effect-registration)
   - [2.3 Query Compilation Pipeline](#23-query-compilation-pipeline)
   - [2.4 Architecture Diagram](#24-architecture-diagram)
3. [Schema Building & DDL Extensions](#3-schema-building--ddl-extensions)
   - [3.1 Extension Lifecycle (`enableExtension` vs `createExtensionIfNotExists`)](#31-extension-lifecycle-enableextension-vs-createextensionifnotexists)
   - [3.2 TableBuilder Extensions (`vector`, `halfvec`, `sparsevec`)](#32-tablebuilder-extensions-vector-halfvec-sparsevec)
   - [3.3 Binary Vector Handling (`table.bit`)](#33-binary-vector-handling-tablebit)
   - [3.4 Type String Generation Utilities (`src/utils.js`)](#34-type-string-generation-utilities-srcutilsjs)
   - [3.5 Index Creation (HNSW & IVFFlat via Knex DDL)](#35-index-creation-hnsw--ivfflat-via-knex-ddl)
4. [Query Building & Distance Operators](#4-query-building--distance-operators)
   - [4.1 QueryBuilder Prototype Extension Breakdown](#41-querybuilder-prototype-extension-breakdown)
   - [4.2 Distance Operator Mappings & Vector Arithmetic](#42-distance-operator-mappings--vector-arithmetic)
   - [4.3 Parameter Escaping & Sanitization (`??` vs `?`)](#43-parameter-escaping--sanitization--vs-)
   - [4.4 Nearest Neighbor Queries (`orderBy`)](#44-nearest-neighbor-queries-orderby)
   - [4.5 Distance Filtering & Threshold Predicates (`whereRaw` / `where`)](#45-distance-filtering--threshold-predicates-whereraw--where)
   - [4.6 Distance Selection & Column Aliasing](#46-distance-selection--column-aliasing)
5. [Type System & TypeScript Module Augmentation](#5-type-system--typescript-module-augmentation)
   - [5.1 Module Augmentation (`declare module 'knex'`)](#51-module-augmentation-declare-module-knex)
   - [5.2 TableBuilder Interface Merging](#52-tablebuilder-interface-merging)
   - [5.3 QueryInterface & QueryBuilder Merging](#53-queryinterface--querybuilder-merging)
   - [5.4 Re-exported Type Declarations](#54-re-exported-type-declarations)
6. [Comprehensive Test Walkthrough (`tests/knex.test.js`)](#6-comprehensive-test-walkthrough-testsknextestjs)
   - [6.1 Database Connection & Extension Initialization](#61-database-connection--extension-initialization)
   - [6.2 Schema Definition Across Vector Flavors](#62-schema-definition-across-vector-flavors)
   - [6.3 Batch Record Insertion & Serialization](#63-batch-record-insertion--serialization)
   - [6.4 Verification Across All Distance Metrics](#64-verification-across-all-distance-metrics)
   - [6.5 Approximate Index Creation & Teardown](#65-approximate-index-creation--teardown)
7. [Knex vs. Kysely Comparison Matrix](#7-knex-vs-kysely-comparison-matrix)
8. [Production Best Practices & Caveats](#8-production-best-practices--caveats)

---

## 1. Executive Summary

`pgvector-node` provides seamless, idiomatic integration with [Knex.js](https://knexjs.org/), the ubiquitous SQL query builder for Node.js. Unlike standalone wrapper libraries that force applications to execute unmanaged raw strings or bypass the query builder, `pgvector/knex` adopts Knex's native plugin architecture (`TableBuilder.extend`, `QueryBuilder.extend`, and `SchemaBuilder.extend`) to mutate the Knex prototypes directly.

Key architectural features include:
- **Global Prototype Extension:** Importing `pgvector/knex` executes registration hooks that attach `table.vector()`, `table.halfvec()`, and `table.sparsevec()` methods to Knex's schema generation pipeline, as well as vector distance calculators (`knex.l2Distance()`, `knex.cosineDistance()`, etc.) to the query construction pipeline.
- **Support for All Vector Variants:** Comprehensive DDL and query support for 32-bit floating point vectors (`vector`), 16-bit half-precision vectors (`halfvec`), sparse vectors (`sparsevec`), and binary quantization bitstrings (`bit`).
- **SQL Injection Prevention:** Query expressions leverage Knex's `client.raw('?? <operator> ?', [column, toSql(value)])` parameter binding, using `??` for identifier qualification and `?` for literal parameter binding.
- **TypeScript Declaration Merging:** Ships complete `.d.ts` module augmentations for Knex's `Knex.TableBuilder` and `Knex.QueryInterface`, ensuring full compile-time autocompletion and type checking in TypeScript projects.

---

## 2. Architectural Mechanics & Extension Model

### 2.1 Knex Prototype Extension Architecture

Knex.js features an extensible core designed around class prototypes:
- `knex.SchemaBuilder`: Constructs DDL statements (`CREATE TABLE`, `ALTER TABLE`, `CREATE EXTENSION`).
- `knex.TableBuilder`: Configures column definitions and table-level constraints inside a table creation callback.
- `knex.QueryBuilder`: Assembles DML queries (`SELECT`, `INSERT`, `UPDATE`, `DELETE`).
- `knex.Client`: Manages dialect-specific compilation, connection pooling, and driver execution (`pg`).

`pgvector-node` uses Knex's public static extension APIs (`extend`) on these constructors rather than monkey-patching hidden internals:

```javascript
knex.TableBuilder.extend('vector', function (name, options) { ... });
knex.QueryBuilder.extend('l2Distance', function (column, value) { ... });
```

When Knex instantiates a query or table builder, these registered methods become first-class methods on the builder instances and are mirrored onto the root `knex` client instance.

### 2.2 Evaluation Lifecycle & Side-Effect Registration

The file `src/knex/index.js` executes side-effects upon module import. Merely executing:

```javascript
import pgvector from 'pgvector/knex';
```

causes the following sequence of events:
1. Knex's `SchemaBuilder`, `TableBuilder`, and `QueryBuilder` prototypes receive extension methods.
2. The core serializer and deserializer functions (`toSql`, `fromSql`) and `SparseVector` class are imported from `../index.js`.
3. The root export exposes `{ fromSql, toSql }` as both named and default exports.

Because JavaScript ES modules execute top-level statements exactly once when imported, importing `pgvector/knex` anywhere in a process lifecycle permanently instruments all Knex instances in that process.

### 2.3 Query Compilation Pipeline

When a developer invokes a distance helper such as `knex.l2Distance('embedding', [1, 2, 3])`, the following transformation occurs:

1. **Input Transformation:** `toSql(value)` converts the JavaScript array `[1, 2, 3]` into a valid PostgreSQL vector string literal `'[1,2,3]'` via `JSON.stringify()`. If a `SparseVector` instance is passed, it invokes `.toPostgres()` (e.g., `'{1:1,2:2}/3'`).
2. **Raw Expression Instantiation:** `this.client.raw('?? <-> ?', [column, toSql(value)])` constructs a Knex `Raw` builder node.
3. **Identifier Escaping:** During query compilation, the dialect compiler (Postgres) escapes `??` using double quotes (`"embedding"`), preventing SQL injection through dynamic column names.
4. **Parameter Binding:** The vector literal `'[1,2,3]'` replaces `?` as a parameterized placeholder (`$1`), ensuring safe transmission over PostgreSQL's extended query protocol.
5. **Clause Embedding:** The compiled `Raw` node is inserted into the SQL AST (e.g., inside an `ORDER BY` or `WHERE` clause).

### 2.4 Architecture Diagram

```mermaid
flowchart TD
    subgraph Client Application
        AppCode["Application Code"]
        ImportStatement["import pgvector from 'pgvector/knex'"]
    end

    subgraph "pgvector/knex" Module
        SideEffects["Execute Prototype Extensions"]
        TableBuilderExt["knex.TableBuilder.extend('vector', ...)"]
        QueryBuilderExt["knex.QueryBuilder.extend('l2Distance', ...)"]
        Serializer["toSql() Serializer"]
    end

    subgraph Knex Core Engine
        TableBuilder["knex.TableBuilder.prototype"]
        QueryBuilder["knex.QueryBuilder.prototype"]
        RawNode["knex.Client.raw('?? <-> ?', [col, val])"]
        QueryCompiler["Knex PostgreSQL QueryCompiler"]
    end

    subgraph PostgreSQL Database
        PostgresDriver["pg (node-postgres)"]
        PGVectorExt["pgvector Engine (<->, <=>, <#>, <+>)"]
    end

    ImportStatement --> SideEffects
    SideEffects --> TableBuilderExt --> TableBuilder
    SideEffects --> QueryBuilderExt --> QueryBuilder

    AppCode -->|"table.vector('embedding', 3)"| TableBuilder
    AppCode -->|"knex.l2Distance('embedding', [1,1,1])"| QueryBuilderExt
    QueryBuilderExt --> Serializer
    Serializer -->|"toSql() -> '[1,1,1]'"| RawNode
    RawNode --> QueryCompiler
    QueryCompiler -->|"SQL: ORDER BY \"embedding\" <-> $1"| PostgresDriver
    PostgresDriver --> PGVectorExt
```

---

## 3. Schema Building & DDL Extensions

### 3.1 Extension Lifecycle (`enableExtension` vs `createExtensionIfNotExists`)

In earlier versions of `pgvector-node`, `enableExtension` was registered onto `knex.SchemaBuilder`:

```javascript
knex.SchemaBuilder.extend('enableExtension', deprecate(function (name) {
  return this.raw('CREATE EXTENSION IF NOT EXISTS ??', [name]);
}, 'enableExtension() is deprecated. Use createExtensionIfNotExists() instead.'));
```

Knex natively added `knex.schema.createExtensionIfNotExists()` in Knex v0.21.17+. Therefore, `pgvector-node` marks `enableExtension` as deprecated via Node.js's native `node:util.deprecate` utility. The canonical method for enabling pgvector in Knex is:

```javascript
await knex.schema.createExtensionIfNotExists('vector');
```

This compiles to:

```sql
CREATE EXTENSION IF NOT EXISTS "vector";
```

### 3.2 TableBuilder Extensions (`vector`, `halfvec`, `sparsevec`)

`src/knex/index.js` registers three custom column builders on `knex.TableBuilder`:

```javascript
knex.TableBuilder.extend('vector', function (name, options) {
  const dimensions = options && (Number.isInteger(options) ? options : options.dimensions);
  return this.specificType(name, vectorType(dimensions));
});

knex.TableBuilder.extend('halfvec', function (name, options) {
  const dimensions = options && (Number.isInteger(options) ? options : options.dimensions);
  return this.specificType(name, halfvecType(dimensions));
});

knex.TableBuilder.extend('sparsevec', function (name, options) {
  const dimensions = options && (Number.isInteger(options) ? options : options.dimensions);
  return this.specificType(name, sparsevecType(dimensions));
});
```

#### Dual Argument Handling
Both an integer literal and an options object are supported:
- **Integer notation:** `table.vector('embedding', 3)`
- **Options object notation:** `table.vector('embedding', { dimensions: 3 })`
- **Unconstrained dimension:** `table.vector('embedding')` (compiles to unconstrained `vector`)

When `table.vector('embedding', { dimensions: 3 })` is called, Knex executes:
```javascript
this.specificType('embedding', 'vector(3)')
```
which compiles into the DDL fragment:
```sql
"embedding" vector(3)
```

### 3.3 Binary Vector Handling (`table.bit`)

Binary vectors for Hamming or Jaccard distance do not require a custom `pgvector` TableBuilder extension because Knex already provides native bitstring support via `table.bit()`:

```javascript
table.bit('binary_embedding', { length: 3 });
```
This compiles to:
```sql
"binary_embedding" bit(3)
```

### 3.4 Type String Generation Utilities (`src/utils.js`)

The column type names are generated by centralized utility functions in `src/utils.js`:

```javascript
function typeWithDimensions(name, dimensions) {
  if (dimensions === undefined || dimensions === null) {
    return name;
  }
  if (!Number.isInteger(dimensions)) {
    throw new Error('expected integer');
  }
  return format('%s(%d)', name, dimensions);
}

export function vectorType(dimensions) {
  return typeWithDimensions('vector', dimensions);
}

export function halfvecType(dimensions) {
  return typeWithDimensions('halfvec', dimensions);
}

export function sparsevecType(dimensions) {
  return typeWithDimensions('sparsevec', dimensions);
}
```

This enforces strict validation: passing a non-integer float or invalid string throws `'expected integer'`, preventing malformed DDL.

### 3.5 Index Creation (HNSW & IVFFlat via Knex DDL)

Knex allows creating secondary indexes with custom access methods and raw operator class expressions via `table.index()`:

```javascript
await knex.schema.alterTable('knex_items', function (table) {
  table.index(
    [knex.raw('embedding vector_l2_ops')],
    'knex_items_embedding_idx',
    'hnsw'
  );
});
```

This generates:

```sql
CREATE INDEX "knex_items_embedding_idx" ON "knex_items" USING hnsw (embedding vector_l2_ops);
```

#### Supported Operator Classes for Knex Indexing:
| Vector Type | Metric | Operator Class | Distance Operator |
| :--- | :--- | :--- | :--- |
| `vector` | L2 / Euclidean | `vector_l2_ops` | `<->` |
| `vector` | Inner Product | `vector_ip_ops` | `<#>` |
| `vector` | Cosine Distance | `vector_cosine_ops` | `<=>` |
| `vector` | L1 / Manhattan | `vector_l1_ops` | `<+>` |
| `halfvec` | L2 / Euclidean | `halfvec_l2_ops` | `<->` |
| `halfvec` | Inner Product | `halfvec_ip_ops` | `<#>` |
| `halfvec` | Cosine Distance | `halfvec_cosine_ops` | `<=>` |
| `halfvec` | L1 / Manhattan | `halfvec_l1_ops` | `<+>` |
| `sparsevec` | L2 / Euclidean | `sparsevec_l2_ops` | `<->` |
| `sparsevec` | Inner Product | `sparsevec_ip_ops` | `<#>` |
| `sparsevec` | Cosine Distance | `sparsevec_cosine_ops`| `<=>` |
| `sparsevec` | L1 / Manhattan | `sparsevec_l1_ops` | `<+>` |
| `bit` | Hamming Distance | `bit_hamming_ops` | `<~>` |
| `bit` | Jaccard Distance | `bit_jaccard_ops` | `<%>` |

#### Custom Index Options (HNSW `m` and `ef_construction`):
To supply index parameters in Knex, pass a raw DDL string:

```javascript
await knex.raw(`
  CREATE INDEX knex_items_hnsw_idx 
  ON knex_items 
  USING hnsw (embedding vector_cosine_ops) 
  WITH (m = 16, ef_construction = 64)
`);
```

---

## 4. Query Building & Distance Operators

### 4.1 QueryBuilder Prototype Extension Breakdown

`src/knex/index.js` attaches six query methods to `knex.QueryBuilder`:

```javascript
knex.QueryBuilder.extend('l2Distance', function (column, value) {
  return this.client.raw('?? <-> ?', [column, toSql(value)]);
});

knex.QueryBuilder.extend('maxInnerProduct', function (column, value) {
  return this.client.raw('?? <#> ?', [column, toSql(value)]);
});

knex.QueryBuilder.extend('cosineDistance', function (column, value) {
  return this.client.raw('?? <=> ?', [column, toSql(value)]);
});

knex.QueryBuilder.extend('l1Distance', function (column, value) {
  return this.client.raw('?? <+> ?', [column, toSql(value)]);
});

knex.QueryBuilder.extend('hammingDistance', function (column, value) {
  return this.client.raw('?? <~> ?', [column, value]);
});

knex.QueryBuilder.extend('jaccardDistance', function (column, value) {
  return this.client.raw('?? <%> ?', [column, value]);
});
```

Because `QueryBuilder.extend` attaches these functions to `knex.QueryBuilder.prototype`, they can be invoked in two ways:
1. **Via the top-level client instance:** `knex.l2Distance('embedding', [1, 2, 3])`
2. **Directly on an existing query builder instance:** `knex('items').l2Distance('embedding', [1, 2, 3])`

In both cases, `this.client.raw(...)` creates a Knex `Raw` object that can be passed into `.orderBy()`, `.where()`, or `.select()`.

### 4.2 Distance Operator Mappings & Vector Arithmetic

| Helper Method | PostgreSQL Operator | Formula / Definition | Type Compatibility |
| :--- | :---: | :--- | :--- |
| `l2Distance(col, val)` | `<->` | Euclidean distance: $\sqrt{\sum (u_i - v_i)^2}$ | `vector`, `halfvec`, `sparsevec` |
| `maxInnerProduct(col, val)` | `<#>` | Negative inner product: $- (u \cdot v)$ | `vector`, `halfvec`, `sparsevec` |
| `cosineDistance(col, val)` | `<=>` | Cosine distance: $1 - \frac{u \cdot v}{\|u\|_2 \|v\|_2}$ | `vector`, `halfvec`, `sparsevec` |
| `l1Distance(col, val)` | `<+>` | Taxicab / Manhattan distance: $\sum \|u_i - v_i\|$ | `vector`, `halfvec`, `sparsevec` |
| `hammingDistance(col, val)` | `<~>` | Bitwise Hamming distance (count of differing bits) | `bit` |
| `jaccardDistance(col, val)` | `<%>` | Jaccard distance: $1 - \frac{\|u \cap v\|}{\|u \cup v\|}$ | `bit` |

> [!NOTE]
> `maxInnerProduct` computes the **negative** inner product (`<#>`). Sorting in ascending order (`ASC`) sorts from most negative to least negative, which corresponds to highest inner product (highest dot product) first. This satisfies PostgreSQL index ordering conventions.

### 4.3 Parameter Escaping & Sanitization (`??` vs `?`)

Notice the explicit bindings syntax:
```javascript
this.client.raw('?? <-> ?', [column, toSql(value)])
```
- **`??` (Double question mark):** Escaped as a SQL **identifier**. Knex replaces this with `"embedding"` (or `"schema"."table"."embedding"` if qualified). This prevents SQL injection through malicious column names.
- **`?` (Single question mark):** Escaped as a SQL **value binding**. Knex passes the string returned by `toSql(value)` as a positional query parameter (`$1`, `$2`, etc.), preventing SQL injection through vector data payloads.

For binary bitstrings (`hammingDistance` and `jaccardDistance`), the raw bitstring `'101'` is passed directly as the parameter value:
```javascript
knex.QueryBuilder.extend('hammingDistance', function (column, value) {
  return this.client.raw('?? <~> ?', [column, value]);
});
```

### 4.4 Nearest Neighbor Queries (`orderBy`)

The canonical k-nearest neighbor (k-NN) query orders by distance and applies a `.limit(k)`:

```javascript
const items = await knex('knex_items')
  .orderBy(knex.l2Distance('embedding', [1, 1, 1]))
  .limit(5);
```

Compiled SQL:
```sql
select * from "knex_items" order by "embedding" <-> $1 asc limit $2
-- Bindings: ['[1,1,1]', 5]
```

### 4.5 Distance Filtering & Threshold Predicates (`whereRaw` / `where`)

To perform radius or threshold searches (find all vectors within distance $\epsilon$):

#### Option A: Using the extended distance helper inside `where`:
```javascript
const items = await knex('knex_items')
  .where(knex.l2Distance('embedding', [1, 1, 1]), '<', 0.5)
  .orderBy(knex.l2Distance('embedding', [1, 1, 1]));
```

Compiled SQL:
```sql
select * from "knex_items" 
where "embedding" <-> $1 < $2 
order by "embedding" <-> $3 asc
-- Bindings: ['[1,1,1]', 0.5, '[1,1,1]']
```

#### Option B: Using `whereRaw` directly:
```javascript
import { toSql } from 'pgvector';

const items = await knex('knex_items')
  .whereRaw('embedding <=> ? < ?', [toSql([1, 1, 1]), 0.5])
  .orderByRaw('embedding <=> ?', [toSql([1, 1, 1])]);
```

### 4.6 Distance Selection & Column Aliasing

To retrieve the calculated distance alongside the entity attributes:

```javascript
const items = await knex('knex_items')
  .select('id')
  .select(knex.l2Distance('embedding', [1, 1, 1]).as('distance'))
  .orderBy('distance', 'asc')
  .limit(10);
```

Compiled SQL:
```sql
select "id", "embedding" <-> $1 as "distance" 
from "knex_items" 
order by "distance" asc 
limit $2
```

---

## 5. Type System & TypeScript Module Augmentation

### 5.1 Module Augmentation (`declare module 'knex'`)

Because Knex is authored in JavaScript and maintains its types in `@types/knex` or directly in `knex`, `pgvector-node` uses TypeScript's **declaration merging** to extend Knex's type declarations.

Located in `src/knex/index.d.ts`:

```typescript
import 'knex';

declare module 'knex' {
  namespace Knex {
    // https://knexjs.org/guide/extending.html
    interface TableBuilder {
      vector(name: string, options: any): Knex.TableBuilder;
      halfvec(name: string, options: any): Knex.TableBuilder;
      sparsevec(name: string, options: any): Knex.TableBuilder;
    }

    // https://knexjs.org/guide/query-builder.html#extending-query-builder
    interface QueryInterface<TRecord extends {} = any, TResult = any> {
      l2Distance(column: any, value: any): Knex.QueryBuilder<TRecord, TResult>;
      maxInnerProduct(column: any, value: any): Knex.QueryBuilder<TRecord, TResult>;
      cosineDistance(column: any, value: any): Knex.QueryBuilder<TRecord, TResult>;
      l1Distance(column: any, value: any): Knex.QueryBuilder<TRecord, TResult>;
      hammingDistance(column: any, value: any): Knex.QueryBuilder<TRecord, TResult>;
      jaccardDistance(column: any, value: any): Knex.QueryBuilder<TRecord, TResult>;
    }
  }
}
```

### 5.2 TableBuilder Interface Merging

By augmenting `Knex.TableBuilder`, any callback inside `knex.schema.createTable('items', (table) => { ... })` gains full TypeScript intellisense for `.vector()`, `.halfvec()`, and `.sparsevec()`.

```typescript
// Compiles cleanly under TypeScript strict mode
knex.schema.createTable('items', (table: Knex.TableBuilder) => {
  table.increments('id');
  table.vector('embedding', { dimensions: 1536 });
  table.halfvec('half_embedding', 768);
  table.sparsevec('sparse_embedding', 10000);
});
```

### 5.3 QueryInterface & QueryBuilder Merging

In Knex, methods available on both `knex(...)` query instances and the root `knex` callable are declared on `Knex.QueryInterface`. By extending `Knex.QueryInterface`, TypeScript recognizes:

```typescript
// Valid: invoked from knex instance
knex.l2Distance('embedding', [1, 2, 3]);

// Valid: invoked within query builder chain
knex('items').l2Distance('embedding', [1, 2, 3]);
```

### 5.4 Re-exported Type Declarations

The entry point `types/knex/index.d.ts` exposes:

```typescript
/// <reference path="../../src/knex/index.d.ts" preserve="true" />
import { fromSql, toSql } from '../index.js';
export { fromSql, toSql };
declare const _default: {
    fromSql: typeof fromSql;
    toSql: typeof toSql;
};
export default _default;
```

This triple-slash reference directive ensures that whenever a TypeScript user imports `pgvector/knex`, the module augmentation in `src/knex/index.d.ts` is automatically included in TypeScript's compilation context.

---

## 6. Comprehensive Test Walkthrough (`tests/knex.test.js`)

The test suite in `tests/knex.test.js` provides an end-to-end integration test validating all data types, distance operators, and index creations.

### 6.1 Database Connection & Extension Initialization

```javascript
const knex = Knex({
  client: 'pg',
  connection: { database: 'pgvector_node_test' }
});

await knex.schema.createExtensionIfNotExists('vector');
```
Connects to the test PostgreSQL database and activates the `vector` extension.

### 6.2 Schema Definition Across Vector Flavors

```javascript
await knex.schema.dropTableIfExists('knex_items');
await knex.schema.createTable('knex_items', (table) => {
  table.increments('id');
  table.vector('embedding', { dimensions: 3 });
  table.halfvec('half_embedding', { dimensions: 3 });
  table.bit('binary_embedding', { length: 3 });
  table.sparsevec('sparse_embedding', { dimensions: 3 });
});
```
Defines four distinct vector columns:
1. `embedding`: 3-dimensional 32-bit float vector.
2. `half_embedding`: 3-dimensional 16-bit half-precision float vector.
3. `binary_embedding`: 3-bit binary vector.
4. `sparse_embedding`: 3-dimensional sparse vector.

### 6.3 Batch Record Insertion & Serialization

```javascript
const newItems = [
  {
    embedding: pgvector.toSql([1, 1, 1]),
    half_embedding: pgvector.toSql([1, 1, 1]),
    binary_embedding: '000',
    sparse_embedding: new SparseVector([1, 1, 1])
  },
  {
    embedding: pgvector.toSql([2, 2, 2]),
    half_embedding: pgvector.toSql([2, 2, 2]),
    binary_embedding: '101',
    sparse_embedding: new SparseVector([2, 2, 2])
  },
  {
    embedding: pgvector.toSql([1, 1, 2]),
    half_embedding: pgvector.toSql([1, 1, 2]),
    binary_embedding: '111',
    sparse_embedding: new SparseVector([1, 1, 2])
  },
  { embedding: null }
];
await knex('knex_items').insert(newItems);
```

#### Crucial Serialization Rules:
- Dense `vector` and `halfvec` values are passed through `pgvector.toSql(arr)`, producing `'[1,1,1]'`.
- `binary_embedding` values are passed as raw bitstrings: `'000'`, `'101'`, `'111'`.
- `sparse_embedding` can be passed directly as a `new SparseVector(...)` instance because `toSql` handles instances of `SparseVector` via `.toPostgres()`.
- Null vectors (`{ embedding: null }`) are handled properly without errors.

### 6.4 Verification Across All Distance Metrics

#### 1. L2 Distance on Standard Vectors:
```javascript
let items = await knex('knex_items')
  .orderBy(knex.l2Distance('embedding', [1, 1, 1]))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);
assert.deepEqual(pgvector.fromSql(items[0].embedding), [1, 1, 1]);
```
- Item 1 (`[1,1,1]`) distance: $0.0$
- Item 3 (`[1,1,2]`) distance: $1.0$
- Item 2 (`[2,2,2]`) distance: $\sqrt{3} \approx 1.732$
- Item 4 (`null`) is sorted last.
- `pgvector.fromSql(items[0].embedding)` parses `'[1,1,1]'` back to `[1, 1, 1]`.

#### 2. L2 Distance on Halfvec and Sparsevec:
```javascript
// halfvec
items = await knex('knex_items')
  .orderBy(knex.l2Distance('half_embedding', [1, 1, 1]))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);

// sparsevec
items = await knex('knex_items')
  .orderBy(knex.l2Distance('sparse_embedding', new SparseVector([1, 1, 1])))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);
```

#### 3. Max Inner Product:
```javascript
items = await knex('knex_items')
  .orderBy(knex.maxInnerProduct('embedding', [1, 1, 1]))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [2, 3, 1, 4]);
```
- Item 2 dot product: $2+2+2 = 6 \implies -6$ (first)
- Item 3 dot product: $1+1+2 = 4 \implies -4$ (second)
- Item 1 dot product: $1+1+1 = 3 \implies -3$ (third)

#### 4. Cosine Distance:
```javascript
items = await knex('knex_items')
  .orderBy(knex.cosineDistance('embedding', [1, 1, 1]))
  .limit(5);
assert.deepEqual(items.map(v => v.id).slice(2), [3, 4]);
```
- Item 1 (`[1,1,1]`) and Item 2 (`[2,2,2]`) point in the identical direction ($\text{cosine distance} = 0.0$).
- Item 3 (`[1,1,2]`) has non-zero cosine distance.

#### 5. L1 Distance:
```javascript
items = await knex('knex_items')
  .orderBy(knex.l1Distance('embedding', [1, 1, 1]))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);
```

#### 6. Hamming & Jaccard on Binary Vectors:
```javascript
// Hamming distance against '101'
items = await knex('knex_items')
  .orderBy(knex.hammingDistance('binary_embedding', '101'))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [2, 3, 1, 4]);

// Jaccard distance against '101'
items = await knex('knex_items')
  .orderBy(knex.jaccardDistance('binary_embedding', '101'))
  .limit(5);
assert.deepEqual(items.map(v => v.id), [2, 3, 1, 4]);
```
- Item 2 (`'101'`): Distance $0$
- Item 3 (`'111'`): Distance $1$
- Item 1 (`'000'`): Distance $2$

### 6.5 Approximate Index Creation & Teardown

```javascript
await knex.schema.alterTable('knex_items', function (table) {
  table.index([knex.raw('embedding vector_l2_ops')], 'knex_items_embedding_idx', 'hnsw');
});

await knex.destroy();
```
Successfully creates an HNSW index on the vector column and closes the Knex pool.

---

## 7. Knex vs. Kysely Comparison Matrix

| Feature / Dimension | Knex.js (`pgvector/knex`) | Kysely (`pgvector/kysely`) |
| :--- | :--- | :--- |
| **Architectural Philosophy** | Prototype mutation (`TableBuilder.extend`, `QueryBuilder.extend`) | Standalone pure functions returning AST `RawBuilder` nodes |
| **Import Method** | `import pgvector from 'pgvector/knex'` (side-effect import) | `import { l2Distance } from 'pgvector/kysely'` (explicit functional import) |
| **Type Safety Strategy** | TypeScript Declaration Merging (`declare module 'knex'`) | Generic compile-time type verification against schema interface |
| **Schema Building DDL** | Native builder extensions: `table.vector('emb', 3)` | Raw SQL expression in DDL: `.addColumn('emb', sql\`vector(3)\`)` |
| **Nearest Neighbor Syntax** | `knex('t').orderBy(knex.l2Distance('emb', q))` | `db.selectFrom('t').orderBy(l2Distance('emb', q))` |
| **Column Qualification** | Uses Knex identifier binding `??` | Uses Kysely AST identifier `sql.ref(column)` |
| **Value Parameterization** | Uses `?` with `toSql(value)` parameter | Uses tagged template interpolation `${toSql(value)}` |
| **Threshold Queries** | `where(knex.l2Distance('emb', q), '<', 0.5)` | `where(l2Distance('emb', q), '<', 0.5)` |
| **Index Creation** | `table.index([knex.raw('emb vector_l2_ops')], 'idx', 'hnsw')` | `db.schema.createIndex('idx').on('t').using('hnsw').expression(sql\`emb vector_l2_ops\`)` |
| **Runtime Overhead** | Minimal (lightweight wrapper around `client.raw`) | Zero runtime overhead (AST expressions compiled directly) |

---

## 8. Production Best Practices & Caveats

1. **Explicit Serialization on Ingestion:**  
   Unlike ORMs with automatic type casting, Knex inserts plain JavaScript objects. Always serialize vector arrays using `pgvector.toSql(embedding)` or pass `SparseVector` objects before calling `.insert()`. Passing a raw JavaScript array without `toSql()` will cause the PostgreSQL driver to treat it as an array literal (`ARRAY[1,2,3]`), causing a PostgreSQL type error (`column "embedding" is of type vector but expression is of type double precision[]`).

2. **Deserialization on Retrieval:**  
   The `pg` driver returns `vector` and `halfvec` columns as formatted text strings (e.g. `'[1,2,3]'`). Always parse retrieved values using `pgvector.fromSql(row.embedding)` to obtain native JavaScript arrays of numbers.

3. **Index Build Performance:**  
   For bulk data loading into Knex applications, load records first and create the HNSW or IVFFlat index afterwards:
   ```javascript
   await knex.batchInsert('items', largeDataset, 500);
   await knex.raw('CREATE INDEX ON items USING hnsw (embedding vector_cosine_ops)');
   ```
   Building the index after data loading significantly reduces indexing time and prevents memory fragmentation.

4. **Iterative Memory Consumption:**  
   When querying large vector datasets using Knex, prefer `.stream()` with asynchronous iterators rather than loading millions of vector results into Node.js heap memory simultaneously.
