## ADDED Requirements

### Requirement: Recall of the arena graph
An HNSW graph built with pgvector's in-memory insertion code inside an arena SHALL reach recall@10 ≥ 0.95 against brute force on embedding-like data (10k × 128-d, m = 16, ef_construction = 64).

#### Scenario: Spike recall
- **WHEN** 200 queries are searched with ef_search = 40
- **THEN** recall@10 is at least 0.95

### Requirement: Position independence and read-only search
A graph arena SHALL be searchable after being copied to a file and mapped read-only at a different address, with results identical to the original mapping, and without writing to the mapping.

#### Scenario: Relocated read-only mapping
- **WHEN** the arena is mapped PROT_READ elsewhere and searched in read-only lock mode
- **THEN** every query returns the same ids and distances as before relocation

### Requirement: Concurrent insert and search
Multiple inserter and searcher threads SHALL operate on one growing arena without data races, and inserters SHALL NOT be starved by searchers.

#### Scenario: Concurrency under ThreadSanitizer
- **WHEN** 4 inserters and 4 searchers run for 10 seconds under TSan
- **THEN** no data race is reported and final recall is at least 0.95

### Requirement: Seal point
Insertion SHALL report "seal needed" exactly when pgvector would switch to its on-disk phase (`memoryUsed + 1 MiB >= memoryTotal`), and never allocate past the arena.

#### Scenario: Small arena
- **WHEN** vectors are inserted into a 4 MiB arena until it fills
- **THEN** insertion returns seal-needed and the graph built so far remains searchable
