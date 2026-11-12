## ADDED Requirements

### Requirement: HNSW collections use a graph
A collection whose index method is `hnsw` SHALL answer approximate searches from a pgvector HNSW
graph over its live vectors. `plan.strategy` SHALL be `hnsw` (no filter) or `hnsw_iterative`
(filter) when the planner chooses the index.

#### Scenario: Large unfiltered search
- **WHEN** a 3,000-vector `vector_l2_ops` collection is searched with `explain: true`
- **THEN** `plan.strategy` is `hnsw`, `plan.exact` is false, and recall@10 against `exact: true` is ≥ 0.9

### Requirement: Operator distances
Approximate results SHALL report the distance of the requested operator on the stored vector
(`<->` L2, `<#>` negative inner product, `<=>` cosine distance), never the index distance, ascending,
ties broken by key.

#### Scenario: L2
- **WHEN** an hnsw `vector_l2_ops` collection returns a hit
- **THEN** its `distance` equals the Euclidean distance, not its square

### Requirement: Iterative scans
`iterative_scan` `off`, `relaxed_order` and `strict_order` SHALL all return results for filtered
searches; if the graph walk finds fewer than `offset + k` candidates, the daemon SHALL fall back to an
exact scan.

#### Scenario: Off mode
- **WHEN** a filtered search sets `iterative_scan: "off"`
- **THEN** the request succeeds and every hit matches the filter

### Requirement: Non-indexable vectors
Zero-norm vectors under cosine SHALL NOT be inserted into the graph and SHALL NOT produce NaN in
responses; vectors that are not in the graph SHALL still be found by an exact scan.

#### Scenario: Zero vectors
- **WHEN** a cosine collection holds zero vectors and is searched
- **THEN** the search returns 200 with finite distances

### Requirement: Liveness
Deleted entities and replaced vector versions SHALL never appear in approximate results.

#### Scenario: Update
- **WHEN** an entity's vector is replaced by the query vector
- **THEN** the next search returns it first with distance 0 and no deleted key appears
