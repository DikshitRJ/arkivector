# pgvector-node: Kysely Integration Deep Dive

> **Document Version:** 1.0.0  
> **Target SDK:** `pgvector-node` (`pgvector/kysely`)  
> **Framework Compatibility:** Kysely 0.27+, 0.28+, 0.29+  
> **Supported PostgreSQL Driver:** `pg` (node-postgres), `postgres` (postgres.js), or any Kysely dialect  
> **Source Files Inspected:**  
> - `src/kysely/index.js`  
> - `types/kysely/index.d.ts`  
> - `src/utils.js`  
> - `src/index.js`  
> - `tests/kysely.test.js`  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Type-Safe SQL Query Building with Kysely](#2-type-safe-sql-query-building-with-kysely)
   - [2.1 What is Kysely?](#21-what-is-kysely)
   - [2.2 Schema Modeling & TypeScript Table Interfaces](#22-schema-modeling--typescript-table-interfaces)
   - [2.3 Functional Non-Intrusive Design Philosophy](#23-functional-non-intrusive-design-philosophy)
   - [2.4 Tagged Template Literal Engine & AST Compilation](#24-tagged-template-literal-engine--ast-compilation)
   - [2.5 Architecture Diagram](#25-architecture-diagram)
3. [Helper Functions & Distance Operators (`src/kysely/index.js`)](#3-helper-functions--distance-operators-srckyselyindexjs)
   - [3.1 Source Code Dissection](#31-source-code-dissection)
   - [3.2 The Role of `sql.ref()` for Identifier Escaping](#32-the-role-of-sqlref-for-identifier-escaping)
   - [3.3 Parameter Serialization via `toSql()`](#33-parameter-serialization-via-tosql)
   - [3.4 Operator Matrix & Distance Semantics](#34-operator-matrix--distance-semantics)
4. [TypeScript Definitions & Type Inference (`types/kysely/index.d.ts`)](#4-typescript-definitions--type-inference-typeskyselyindexdts)
   - [4.1 Dissection of `types/kysely/index.d.ts`](#41-dissection-of-typeskyselyindexdts)
   - [4.2 The `RawBuilder<unknown>` Return Type](#42-the-rawbuilderunknown-return-type)
   - [4.3 Polymorphic Value Parameter Typing](#43-polymorphic-value-parameter-typing)
   - [4.4 Compile-Time Verification in Kysely Query Chains](#44-compile-time-verification-in-kysely-query-chains)
5. [Comprehensive Test Walkthrough (`tests/kysely.test.js`)](#5-comprehensive-test-walkthrough-testskyselytestjs)
   - [5.1 PostgresDialect Initialization](#51-postgresdialect-initialization)
   - [5.2 Extension Activation](#52-extension-activation)
   - [5.3 DDL Schema Creation via `sql` Expressions](#53-ddl-schema-creation-via-sql-expressions)
   - [5.4 Multi-Vector Ingestion](#54-multi-vector-ingestion)
   - [5.5 Nearest Neighbor Queries Across Vector Flavors](#55-nearest-neighbor-queries-across-vector-flavors)
   - [5.6 Radius / Distance Threshold Filtering](#56-radius--distance-threshold-filtering)
   - [5.7 HNSW Index Creation](#57-hnsw-index-creation)
6. [Advanced Query Patterns & Real-World Usage](#6-advanced-query-patterns--real-world-usage)
   - [6.1 Distance Selection with Expression Aliasing](#61-distance-selection-with-expression-aliasing)
   - [6.2 Hybrid Search: Metadata Predicates + Vector Distance](#62-hybrid-search-metadata-predicates--vector-distance)
   - [6.3 Custom Serialization Plugins / Transformer Pattern](#63-custom-serialization-plugins--transformer-pattern)
7. [Side-by-Side Comparison: Kysely vs. Knex](#7-side-by-side-comparison-kysely-vs-knex)
8. [Production Best Practices & Caveats](#8-production-best-practices--caveats)

---

## 1. Executive Summary

[Kysely](https://kysely.dev/) (pronounced *"Key-seh-lee"*, Finnish for *"question"* or *"query"*) is a modern, TypeScript-first, zero-runtime-overhead SQL query builder. Unlike traditional ORMs or older query builders like Knex, Kysely relies heavily on TypeScript's type system to validate table names, column identifiers, joins, and return shapes strictly at compile time.

The `pgvector-node` integration (`pgvector/kysely`) provides first-class support for pgvector within Kysely applications. Rather than altering Kysely's prototypes or injecting global state, `pgvector/kysely` is designed as a set of pure, functional expression generators (`l2Distance`, `cosineDistance`, `maxInnerProduct`, `l1Distance`, `hammingDistance`, and `jaccardDistance`) returning Kysely `RawBuilder` instances.

Key highlights:
- **Zero Prototype Pollution:** Completely pure helper functions with no side-effects or monkey-patching.
- **SQL Injection Safety:** Employs Kysely's `sql.ref(column)` to escape column names as SQL identifiers and serializes query vectors via `toSql()` as parameterized bind values.
- **Full Vector Modality Support:** Seamlessly handles dense float32 (`vector`), float16 (`halfvec`), sparse (`sparsevec`), and binary bitstring (`bit`) vectors.
- **Composable Expressions:** Helpers output `RawBuilder<unknown>`, which plug effortlessly into `.orderBy()`, `.where()`, `.select()`, and `.having()` clauses.

---

## 2. Type-Safe SQL Query Building with Kysely

### 2.1 What is Kysely?

Kysely combines the ergonomics of a fluent query builder with the type safety of an ORM without runtime reflection or heavy metadata overhead. In Kysely:
- All database tables and columns are defined via pure TypeScript interfaces.
- The `Kysely<Database>` instance tracks schema types across all query operations.
- Selecting non-existent columns or mistyping table names results in compile-time errors.
- Query methods compile directly into parameterized SQL strings (`sql` and `parameters`).

### 2.2 Schema Modeling & TypeScript Table Interfaces

In a TypeScript application using Kysely and `pgvector`, vector columns can be modeled in database interfaces:

```typescript
import { Generated } from 'kysely';

export interface Database {
  items: ItemTable;
}

export interface ItemTable {
  id: Generated<number>;
  embedding: string | null;            // vector(1536) stored as formatted text in wire protocol
  half_embedding?: string | null;       // halfvec(768)
  binary_embedding?: string | null;     // bit(256)
  sparse_embedding?: string | null;     // sparsevec(10000)
  category: string;
  created_at: Generated<Date>;
}
```

Because node PostgreSQL drivers (`pg` and `postgres.js`) receive vector data from PostgreSQL as formatted strings (e.g. `'[0.023, -0.441, ...]'`), typing the column as `string | null` accurately reflects the runtime type returned by Kysely query executions.

### 2.3 Functional Non-Intrusive Design Philosophy

In contrast to Knex (which extends prototypes globally via `knex.QueryBuilder.extend`), Kysely promotes pure functional composition. `pgvector/kysely` embraces this philosophy:

```javascript
import { l2Distance, cosineDistance } from 'pgvector/kysely';
```

Every function is a pure factory that accepts a column identifier and a vector payload, returning an immutable AST node (`RawBuilder`). This guarantees complete isolation between multiple database instances or multi-tenant configurations.

### 2.4 Tagged Template Literal Engine & AST Compilation

Kysely utilizes tagged template literals (`sql`...``) to safely construct SQL fragments. Inside an `sql` template literal:
- Strings passed through `${sql.ref(column)}` are compiled into quoted SQL identifiers (e.g., `"embedding"`).
- Values passed through `${toSql(value)}` are extracted as bind parameters (e.g., `$1`).
- Operators and raw syntax (such as `<->`, `<=>`, `<#>`) remain static SQL tokens in the compiled abstract syntax tree.

### 2.5 Architecture Diagram

```mermaid
flowchart TD
    subgraph Client Application
        AppQuery["db.selectFrom('items')\n  .orderBy(l2Distance('embedding', [1, 2, 3]))\n  .limit(5)"]
    end

    subgraph "pgvector/kysely" Layer
        L2Helper["l2Distance(column, value)"]
        ToSql["toSql(value) -> '[1,2,3]'"]
        SqlRef["sql.ref('embedding')"]
        RawBuilder["RawBuilder Node:\nsql`${sql.ref(column)} <-> ${toSql(value)}`"]
    end

    subgraph Kysely Core Engine
        AST["Query AST / Node Tree"]
        DialectCompiler["PostgresQueryCompiler"]
        CompiledQuery["CompiledQuery:\n- sql: select * from \"items\" order by \"embedding\" <-> $1 limit $2\n- parameters: ['[1,2,3]', 5]"]
    end

    subgraph Database Execution
        Driver["pg / postgres.js Driver"]
        PostgresEngine["PostgreSQL pgvector Engine"]
    end

    AppQuery --> L2Helper
    L2Helper --> ToSql
    L2Helper --> SqlRef
    ToSql & SqlRef --> RawBuilder
    RawBuilder --> AST
    AST --> DialectCompiler
    DialectCompiler --> CompiledQuery
    CompiledQuery --> Driver
    Driver --> PostgresEngine
```

---

## 3. Helper Functions & Distance Operators (`src/kysely/index.js`)

### 3.1 Source Code Dissection

The entire implementation of `src/kysely/index.js` is concise, robust, and functional:

```javascript
import { sql } from 'kysely';
import { fromSql, toSql } from '../index.js';

/** @import { SparseVector } from '../index.js' */

/**
 * @param {any} column
 * @param {number[] | SparseVector | null} value
 */
export function l2Distance(column, value) {
  return sql`${sql.ref(column)} <-> ${toSql(value)}`;
}

/**
 * @param {any} column
 * @param {number[] | SparseVector | null} value
 */
export function maxInnerProduct(column, value) {
  return sql`${sql.ref(column)} <#> ${toSql(value)}`;
}

/**
 * @param {any} column
 * @param {number[] | SparseVector | null} value
 */
export function cosineDistance(column, value) {
  return sql`${sql.ref(column)} <=> ${toSql(value)}`;
}

/**
 * @param {any} column
 * @param {number[] | SparseVector | null} value
 */
export function l1Distance(column, value) {
  return sql`${sql.ref(column)} <+> ${toSql(value)}`;
}

/**
 * @param {any} column
 * @param {string} value
 */
export function hammingDistance(column, value) {
  return sql`${sql.ref(column)} <~> ${value}`;
}

/**
 * @param {any} column
 * @param {string} value
 */
export function jaccardDistance(column, value) {
  return sql`${sql.ref(column)} <%> ${value}`;
}

export { fromSql, toSql };

export default { fromSql, toSql };
```

### 3.2 The Role of `sql.ref()` for Identifier Escaping

A critical security and correctness detail is the usage of `sql.ref(column)`:

```javascript
return sql`${sql.ref(column)} <-> ${toSql(value)}`;
```

In SQL, column names cannot be passed as parameterized query arguments (e.g. `$1 <-> $2` is invalid when referring to a column in PostgreSQL). However, naive string interpolation (`sql`${column} <-> ...``) would introduce severe SQL injection vulnerabilities if user input reached the `column` parameter.

`sql.ref(column)` tells Kysely's dialect compiler to treat the input strictly as a database **identifier**:
- A string `'embedding'` compiles to `"embedding"`.
- A table-qualified identifier `'items.embedding'` compiles to `"items"."embedding"`.
- Any special characters or SQL injection payloads are escaped using PostgreSQL identifier quoting rules.

### 3.3 Parameter Serialization via `toSql()`

The second interpolation `${toSql(value)}` serializes the vector into a standard PostgreSQL text representation before handing it to Kysely:
- If `value` is an array (`[1, 2, 3]`), `toSql` converts it via `JSON.stringify(value.map(Number))` to `'[1,2,3]'`.
- If `value` is a `SparseVector`, `toSql` invokes `.toPostgres()`, returning `'{1:1,2:2}/3'`.
- If `value` is `null`, `null` is bound.

Because `${toSql(value)}` is passed directly as an interpolation variable into Kysely's `sql` tagged template, Kysely automatically treats it as a **parameter value** and places it into the query's parameter bindings array.

### 3.4 Operator Matrix & Distance Semantics

| Function Name | Infix Operator | Mathematical Definition | PostgreSQL Operator Class | Supported Vector Types |
| :--- | :---: | :--- | :--- | :--- |
| `l2Distance` | `<->` | Euclidean distance ($\|u - v\|_2$) | `vector_l2_ops`, `halfvec_l2_ops`, `sparsevec_l2_ops` | `vector`, `halfvec`, `sparsevec` |
| `maxInnerProduct` | `<#>` | Negative inner product ($- u \cdot v$) | `vector_ip_ops`, `halfvec_ip_ops`, `sparsevec_ip_ops` | `vector`, `halfvec`, `sparsevec` |
| `cosineDistance` | `<=>` | Cosine distance ($1 - \frac{u \cdot v}{\|u\|_2 \|v\|_2}$) | `vector_cosine_ops`, `halfvec_cosine_ops`, `sparsevec_cosine_ops` | `vector`, `halfvec`, `sparsevec` |
| `l1Distance` | `<+>` | Taxicab / Manhattan ($\sum \|u_i - v_i\|$) | `vector_l1_ops`, `halfvec_l1_ops`, `sparsevec_l1_ops` | `vector`, `halfvec`, `sparsevec` |
| `hammingDistance` | `<~>` | Bitwise Hamming distance | `bit_hamming_ops` | `bit` |
| `jaccardDistance` | `<%>` | Jaccard distance | `bit_jaccard_ops` | `bit` |

---

## 4. TypeScript Definitions & Type Inference (`types/kysely/index.d.ts`)

### 4.1 Dissection of `types/kysely/index.d.ts`

```typescript
import { fromSql, toSql } from '../index.js';
import type { SparseVector } from '../index.js';
export declare function l2Distance(column: any, value: number[] | SparseVector | null): import("kysely").RawBuilder<unknown>;
export declare function maxInnerProduct(column: any, value: number[] | SparseVector | null): import("kysely").RawBuilder<unknown>;
export declare function cosineDistance(column: any, value: number[] | SparseVector | null): import("kysely").RawBuilder<unknown>;
export declare function l1Distance(column: any, value: number[] | SparseVector | null): import("kysely").RawBuilder<unknown>;
export declare function hammingDistance(column: any, value: string): import("kysely").RawBuilder<unknown>;
export declare function jaccardDistance(column: any, value: string): import("kysely").RawBuilder<unknown>;
export { fromSql, toSql };
declare const _default: {
    fromSql: typeof fromSql;
    toSql: typeof toSql;
};
export default _default;
```

### 4.2 The `RawBuilder<unknown>` Return Type

Every distance function returns `RawBuilder<unknown>`. In Kysely, `RawBuilder<T>` represents a raw SQL expression snippet whose evaluation results in a value of type `T`.

Because distance calculations yield a floating-point number in PostgreSQL, `RawBuilder` functions can be supplied anywhere Kysely expects a generic SQL expression:
- **`orderBy(expression)`:** Kysely accepts `RawBuilder` as an ordering expression.
- **`where(expression, operator, value)`:** Kysely accepts `RawBuilder` as the left operand in binary comparisons.
- **`select(expression.as(alias))`:** `RawBuilder` can be aliased and projected into the returned record set.

### 4.3 Polymorphic Value Parameter Typing

Notice the parameter types:
- Dense & Sparse distance functions (`l2Distance`, `cosineDistance`, etc.) accept:
  ```typescript
  value: number[] | SparseVector | null
  ```
- Binary distance functions (`hammingDistance`, `jaccardDistance`) accept:
  ```typescript
  value: string
  ```
  This enforces at compile-time that callers pass valid bitstrings (e.g. `'101'`) rather than numeric arrays.

### 4.4 Compile-Time Verification in Kysely Query Chains

In an end-user application, TypeScript enforces the structure:

```typescript
// Type checks cleanly:
const result = await db.selectFrom('items')
  .selectAll()
  .orderBy(cosineDistance('embedding', [0.1, 0.2, 0.3]))
  .limit(5)
  .execute();

// Passing a string to l2Distance generates a compile error:
// @ts-expect-error - string is not assignable to number[] | SparseVector | null
l2Distance('embedding', "invalid-vector");
```

---

## 5. Comprehensive Test Walkthrough (`tests/kysely.test.js`)

The test suite in `tests/kysely.test.js` covers every facet of Kysely integration.

### 5.1 PostgresDialect Initialization

```javascript
import pg from 'pg';
import { Kysely, PostgresDialect, sql } from 'kysely';

const dialect = new PostgresDialect({
  pool: new pg.Pool({
    database: 'pgvector_node_test'
  })
});

const db = new Kysely({
  dialect
});
```

Kysely is decoupled from database drivers. The `PostgresDialect` connects Kysely's query compiler to `node-postgres` (`pg.Pool`).

### 5.2 Extension Activation

```javascript
await sql`CREATE EXTENSION IF NOT EXISTS vector`.execute(db);
```
Activates the `vector` PostgreSQL extension using an executed `sql` tagged template literal.

### 5.3 DDL Schema Creation via `sql` Expressions

```javascript
await db.schema.dropTable('kysely_items')
  .ifExists()
  .execute();

await db.schema.createTable('kysely_items')
  .addColumn('id', 'serial', (cb) => cb.primaryKey())
  .addColumn('embedding', sql`vector(3)`)
  .addColumn('half_embedding', sql`halfvec(3)`)
  .addColumn('binary_embedding', sql`bit(3)`)
  .addColumn('sparse_embedding', sql`sparsevec(3)`)
  .execute();
```

Kysely's schema builder uses `.addColumn(name, type)`. Because vector types are custom PostgreSQL types, passing `sql\`vector(3)\``, `sql\`halfvec(3)\``, `sql\`bit(3)\``, and `sql\`sparsevec(3)\`` emits the exact custom column specifications in the generated `CREATE TABLE` statement.

### 5.4 Multi-Vector Ingestion

```javascript
import pgvector from 'pgvector/kysely';
import { SparseVector } from 'pgvector';

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

await db.insertInto('kysely_items')
  .values(newItems)
  .execute();
```

Values are converted to database strings using `pgvector.toSql(...)` for dense vectors, raw strings for bitvectors, and `SparseVector` instances for sparse vectors.

### 5.5 Nearest Neighbor Queries Across Vector Flavors

#### L2 Distance (Dense Vector):
```javascript
let items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(l2Distance('embedding', [1, 1, 1]))
  .limit(5)
  .execute();

assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);
assert.deepEqual(pgvector.fromSql(items[0].embedding), [1, 1, 1]);
```

#### L2 Distance (Halfvec & Sparsevec):
```javascript
// halfvec
items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(l2Distance('half_embedding', [1, 1, 1]))
  .limit(5)
  .execute();
assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);

// sparsevec
items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(l2Distance('sparse_embedding', new SparseVector([1, 1, 1])))
  .limit(5)
  .execute();
assert.deepEqual(items.map(v => v.id), [1, 3, 2, 4]);
```

#### Max Inner Product & Cosine Distance:
```javascript
// max inner product (<#>)
items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(maxInnerProduct('embedding', [1, 1, 1]))
  .limit(5)
  .execute();
assert.deepEqual(items.map(v => v.id), [2, 3, 1, 4]);

// cosine distance (<=>)
items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(cosineDistance('embedding', [1, 1, 1]))
  .limit(5)
  .execute();
assert.deepEqual(items.map(v => v.id).slice(2), [3, 4]);
```

#### Hamming & Jaccard on Binary Quantized Vectors:
```javascript
// Hamming distance (<~>)
items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(hammingDistance('binary_embedding', '101'))
  .limit(5)
  .execute();
assert.deepEqual(items.map(v => v.id), [2, 3, 1, 4]);

// Jaccard distance (<%>)
items = await db.selectFrom('kysely_items')
  .selectAll()
  .orderBy(jaccardDistance('binary_embedding', '101'))
  .limit(5)
  .execute();
assert.deepEqual(items.map(v => v.id), [2, 3, 1, 4]);
```

### 5.6 Radius / Distance Threshold Filtering

Kysely supports complex filtering predicates via `.where()`:

```javascript
items = await db.selectFrom('kysely_items')
  .selectAll()
  .where(l2Distance('embedding', [1, 1, 1]), '<', 0.5)
  .execute();

assert.deepEqual(items.map(v => v.id), [1]);
```

This compiles cleanly to:
```sql
select * from "kysely_items" where "embedding" <-> $1 < $2
-- Bindings: ['[1,1,1]', 0.5]
```

### 5.7 HNSW Index Creation

Kysely provides a fluent index creation API (`db.schema.createIndex`):

```javascript
await db.schema.createIndex('kysely_items_embedding_idx')
  .on('kysely_items')
  .using('hnsw')
  .expression(sql`embedding vector_l2_ops`)
  .execute();
```

Compiled DDL:
```sql
create index "kysely_items_embedding_idx" on "kysely_items" using hnsw (embedding vector_l2_ops)
```

---

## 6. Advanced Query Patterns & Real-World Usage

### 6.1 Distance Selection with Expression Aliasing

To project the distance score alongside row attributes:

```typescript
const queryVector = [0.12, 0.45, -0.67];

const records = await db.selectFrom('items')
  .select([
    'id',
    'category',
    cosineDistance('embedding', queryVector).as('distance')
  ])
  .where('category', '=', 'electronics')
  .orderBy('distance', 'asc')
  .limit(10)
  .execute();

for (const row of records) {
  console.log(`Item ${row.id}: distance = ${row.distance}`);
}
```

### 6.2 Hybrid Search: Metadata Predicates + Vector Distance

Kysely's strengths become apparent when building hybrid searches combining relational filters, full-text search, and vector similarity:

```typescript
const results = await db.selectFrom('documents')
  .selectAll()
  .where('status', '=', 'published')
  .where('published_at', '>=', new Date('2025-01-01'))
  .where((eb) => eb.or([
    eb('tags', 'like', '%ai%'),
    eb('tags', 'like', '%vectors%')
  ]))
  .orderBy(cosineDistance('embedding', queryEmbedding))
  .limit(20)
  .execute();
```

PostgreSQL's planner will evaluate the relational predicates and use the HNSW index to fetch top matches efficiently.

### 6.3 Custom Serialization Plugins / Transformer Pattern

For applications that prefer working with native `number[]` arrays in their TypeScript models instead of raw strings, Kysely plugins (such as `KyselyPlugin`) can be used to automatically intercept results:

```typescript
import { KyselyPlugin, PluginTransformQueryArgs, PluginTransformResultArgs, RootOperationNode, QueryResult } from 'kysely';
import { fromSql } from 'pgvector';

export class VectorDeserializerPlugin implements KyselyPlugin {
  transformQuery(args: PluginTransformQueryArgs): RootOperationNode {
    return args.node;
  }

  async transformResult(args: PluginTransformResultArgs): Promise<QueryResult<any>> {
    const rows = args.result.rows.map((row) => {
      if (row && typeof row.embedding === 'string') {
        return {
          ...row,
          embedding: fromSql(row.embedding)
        };
      }
      return row;
    });

    return { ...args.result, rows };
  }
}
```

Adding this plugin to the Kysely instance transparently converts `'[1,2,3]'` into `[1, 2, 3]` across all query results.

---

## 7. Side-by-Side Comparison: Kysely vs. Knex

| Feature / Dimension | Kysely (`pgvector/kysely`) | Knex (`pgvector/knex`) |
| :--- | :--- | :--- |
| **Paradigm** | Pure functional functions returning AST expressions | Prototype monkey-patching via Knex plugin extensions |
| **Type Checking** | Strict compile-time validation of tables, columns, and expressions | Ambient declaration merging (`declare module 'knex'`) |
| **State & Scope** | Completely stateless, zero global side-effects | Modifies shared `knex.QueryBuilder` and `knex.TableBuilder` |
| **Query Syntax** | `.orderBy(cosineDistance('embedding', val))` | `.orderBy(knex.cosineDistance('embedding', val))` |
| **Identifier Escaping** | `sql.ref(column)` AST node | `??` Knex raw token binding |
| **Parameter Binding** | `${toSql(val)}` tagged template interpolation | `?` Knex raw token binding |
| **Table DDL Creation** | `.addColumn('col', sql\`vector(3)\`)` | `table.vector('col', 3)` |
| **Index Creation** | `.using('hnsw').expression(sql\`col vector_l2_ops\`)` | `table.index([knex.raw('col vector_l2_ops')], 'idx', 'hnsw')` |
| **Runtime Performance** | Zero overhead; template literals compile directly to SQL | Negligible overhead (allocates Knex Raw builder instances) |
| **Driver Flexibility** | Agnostic: supports `pg`, `postgres.js`, `libsql`, `kysely-pgvector` | Coupled to Knex's internal driver dialect system (`pg`) |

---

## 8. Production Best Practices & Caveats

1. **Always Qualify Columns in Joins:**  
   When performing vector similarity across joined tables, pass qualified identifiers to the helper to prevent ambiguous column errors:
   ```typescript
   db.selectFrom('items')
     .innerJoin('item_embeddings', 'item_embeddings.item_id', 'items.id')
     .orderBy(l2Distance('item_embeddings.embedding', queryVector))
   ```
   `sql.ref('item_embeddings.embedding')` correctly splits and quotes each identifier: `"item_embeddings"."embedding"`.

2. **Index Optimization Parameters:**  
   When creating HNSW indexes with high recall requirements, include `m` and `ef_construction` in the DDL expression:
   ```typescript
   await db.schema.createIndex('items_embedding_hnsw_idx')
     .on('items')
     .using('hnsw')
     .expression(sql`embedding vector_cosine_ops WITH (m = 24, ef_construction = 128)`)
     .execute();
   ```

3. **Setting `hnsw.ef_search` per Session / Transaction:**  
   To optimize recall at query time, execute a transaction-local `SET` command before querying:
   ```typescript
   await db.transaction().execute(async (trx) => {
     await sql`SET LOCAL hnsw.ef_search = 100`.execute(trx);
     return trx.selectFrom('items')
       .selectAll()
       .orderBy(cosineDistance('embedding', queryVector))
       .limit(10)
       .execute();
   });
   ```

4. **Handling Null Vectors:**  
   In PostgreSQL, `<->` against a `NULL` column produces `NULL`. PostgreSQL sorts `NULL` values according to `NULLS FIRST` or `NULLS LAST` rules. To prevent `NULL` embeddings from appearing unexpectedly, add `.where('embedding', 'is not', null)` to nearest-neighbor queries.
