# Arkiv Research & Documentation Execution Plan

## 1. Executive Summary

This plan outlines the systematic reverse-engineering, analysis, and comprehensive architectural documentation of **Arkiv** (`https://docs.arkiv.network`), a next-generation Web3 database built atop EVM state with native indexing, time-to-live (TTL) expiration, Merkle-Patricia trie commitment, and JSON-RPC query capabilities.

All research is conducted on branch `research/arkiv-docs` inside directory `RESEARCH/arkiv-docs/`. The objective is to document every protocol layer, endpoint, contract function, and data structure with extreme precision and academic rigor, providing the ultimate blueprint for implementing an idiomatic Arkiv SDK in any programming language (TypeScript, Rust, Go, Python).

---

## 2. Arkiv Documentation Surface & Ingested Corpus

The documentation corpus has been extracted into raw local markdown files located at:
`~/.gemini/antigravity-cli/brain/573f050b-b382-4a3d-9972-8dc6e94008f1/scratch/raw-docs/md/`

### Scraped Sources:
1. `start-here-introduction.md` – Core philosophy, four principles, $ fields, public nature.
2. `start-here-first-entity.md` – Practical tutorial, schema definition, create/query workflows.
3. `start-here-clients.md` – Public vs Wallet client separation, transport layers.
4. `start-here-access-keys.md` – Hub authentication, rate limiting, quota tracking, origin/IP allowlists.
5. `start-here-agent-skill.md` – Official Anthropic agent skills (`arkiv-best-practices`, `arkiv-feedback`).
6. `schemas-define.md` – Schema DSL, `.indexed()`, `.optional()`, `.default()`, naming rules, tag isolation.
7. `schemas-column-types.md` – 10 supported column types, storage sizes, indexing constraints.
8. `writing-create.md` – Single entity creation, salt generation, expiry parameters, fee estimation.
9. `writing-expiration.md` – Time resolution (`max(expiresAt, currentBlock + minLifetime)`), permissionless extension.
10. `writing-change.md` – Patching mutations, ownership transfers, entity deletions, tombstones.
11. `writing-batches.md` – Atomic multi-operation batches (`executeBatch`), salt pre-allocation, cross-entity key references.
12. `querying-select.md` – Projection (`select`), entity fields vs attributes, wildcard `*`, `$attributes` map.
13. `querying-filter.md` – Predicates (`eq`, `between`, `startsWith`, `exists`, `hasType`), `$owner`/`$creator` trust boundaries.
14. `querying-sort.md` – `ORDER BY attribute:type ASC|DESC`, sorting unindexed columns, tie handling.
15. `querying-paginate.md` – Offset pagination vs key-cursor pagination (`$key > key(0x...)`), page boundaries.
16. `querying-count.md` – Aggregate count queries, headless count execution.
17. `querying-get-entity.md` – Single entity retrieval by 32-byte key.
18. `querying-without-schema.md` – Dynamic entity queries, `$attributeSchema` discovery, schema-less reads.
19. `querying-watch-events.md` – Real-time event polling, log subscription, unwatch callbacks.
20. `reference-query-language.md` – AST grammar, SQL-like syntax, expressions, literals, reserved words.
21. `reference-errors.md` – Comprehensive error catalog (SDK, JSON-RPC codes, EVM custom errors).
22. `reference-limits.md` – Hard protocol limits (32 attributes, 128 bytes str, 128 KiB bytes, 7.88M blocks TTL).
23. `json-rpc-querying-data.md` – Read methods (`arkiv_query`, `arkiv_getEntity`, `arkiv_getBlockTiming`).
24. `json-rpc-mutating-entities.md` – EVM write transactions, `execute(ops)` calldata encoding, key derivation formula.
25. `networks-rabdi.md` – Rabdi public testnet parameters (Chain ID 7794095, RPCs, block time 2s).
26. `networks-older-networks.md` – Deprecated testnets (Tiramisu, etc.).
27. `guides-react.md` – React hooks, TanStack Query patterns, Wagmi wallet integration.

---

## 3. Target Directory & File Architecture

The research artifacts will be systematically structured within `RESEARCH/arkiv-docs/` as follows:

```
RESEARCH/arkiv-docs/
├── plan.md                                    # This document: master execution & research plan
├── README.md                                  # Architectural overview: How Arkiv backend is invoked (dual-rail protocol)
├── WHAT_WE_LEARN.md                           # Engineering conclusions, mental models, trade-offs, SDK thesis
├── 01-architecture-and-fundamentals/
│   ├── overview.md                            # What Arkiv is, four principles, Merkle-Patricia state, gas model
│   ├── entity-model.md                        # Entity anatomy, $ system fields ($key, $owner, $creator, etc.)
│   ├── attributes-and-types.md                # 10 Column types, typeId encoding, bit 7 index flag, byte alignment
│   ├── lifecycle-and-expiration.md            # Block-based TTL, minLifetime vs expiresAt, extension rules
│   └── network-and-infrastructure.md          # Rabdi testnet (ChainID 7794095), RPC endpoints, access key security
├── 02-json-rpc-protocol/
│   ├── overview.md                            # Dual-rail protocol: JSON-RPC reads vs EVM writes
│   ├── arkiv_query.md                         # Detailed specification of arkiv_query, params, projection, count
│   ├── arkiv_getEntity.md                     # Direct 32-byte key lookup, schema-less raw retrieval, null returns
│   ├── arkiv_getBlockTiming.md                # Node block height, unix timestamp, block duration
│   └── error-handling.md                      # JSON-RPC error codes (-32001 to -32004, -32602)
├── 03-evm-mutations-and-contract-spec/
│   ├── execute-function.md                    # Contract address 0x44..44, execute(ops), selector 0x49650044
│   ├── operation-types.md                     # Ops 1-5 (CREATE, PATCH, EXTEND, TRANSFER, DELETE) ABI structs
│   ├── attribute-serialization.md             # struct Attribute, ASCII name packing, 32-attribute cap, sorting
│   ├── entity-key-derivation.md               # keccak256 formula, entityNonce eth_call (0x36917bfd), salt predictability
│   └── events-and-logs.md                     # EntityCreated, EntityPatched, ExpiryExtended, etc., log indexing
├── 04-query-engine-and-ast/
│   ├── query-language-grammar.md              # EBNF grammar, expressions, binary operators, keywords, comments
│   ├── operators-and-predicates.md            # eq, ne, gt, gte, lt, lte, between, startsWith, exists, hasType
│   ├── ordering-and-sorting.md                # ORDER BY attribute:type ASC/DESC, unindexed sort, null handling
│   ├── pagination-and-cursors.md              # Offset pagination vs key-cursor pagination ($key > key(0x...))
│   └── schema-filtering-tag.md                # Tag isolation (arkiv.schema = str('...')), skipped entity handling
├── 05-sdk-architecture-blueprint/
│   ├── sdk-design-thesis.md                   # Core thesis: Building a production-grade Arkiv SDK
│   ├── client-abstractions.md                 # ArkivPublicClient vs ArkivWalletClient interfaces
│   ├── query-builder-dsl.md                   # Type-safe fluent query builder, AST generation, SQL serialization
│   ├── schema-declaration-system.md           # defineSchema DSL, runtime column validation, type inference
│   ├── batch-pipeline-orchestrator.md         # executeBatch builder, salt generation, key prediction pipelines
│   └── react-and-framework-bindings.md        # TanStack Query integration, event subscriptions, optimistic UI
└── 06-reference-and-benchmarks/
    ├── limits-and-quotas.md                   # Protocol limits (128 bytes str, 128 KiB bytes, 32 attributes, etc.)
    ├── complete-error-catalog.md              # Cross-reference of SDK, RPC, and EVM contract reverts
    └── comparison-with-web3-storage.md        # Comparison with IPFS, Arweave, Ceramic, Filecoin, Tableland
```

---

## 4. Phase Breakdown & Subagent Delegation Matrix

| Phase | Milestone Description | Subagent Specialization | Target Output Files | Git Commit Message |
|---|---|---|---|---|
| **Phase 1** | Ingestion, Corpus Parsing & Master Execution Plan | Parent Orchestrator | `plan.md` | `docs(research): initialize arkiv research plan and roadmap` |
| **Phase 2** | Architecture & Core Fundamentals | Subagent `arch-specialist` (Gemini Flash) | `01-architecture-and-fundamentals/*.md` | `docs(research): add architecture, entity model, types and network fundamentals` |
| **Phase 3** | JSON-RPC Read Protocol & Query Endpoints | Subagent `rpc-specialist` (Gemini Flash) | `02-json-rpc-protocol/*.md` | `docs(research): document json-rpc endpoints, read methods, and error specs` |
| **Phase 4** | EVM Mutations & Smart Contract Specification | Subagent `evm-specialist` (Gemini Flash) | `03-evm-mutations-and-contract-spec/*.md` | `docs(research): document evm transaction mutations, execute calldata and key derivation` |
| **Phase 5** | Query Engine, AST Grammar & Predicates | Subagent `query-specialist` (Gemini Flash) | `04-query-engine-and-ast/*.md` | `docs(research): document query engine, predicates, ast and pagination` |
| **Phase 6** | SDK Architecture Blueprint & Implementation Guide | Subagent `sdk-architect` (Gemini Flash) | `05-sdk-architecture-blueprint/*.md` | `docs(research): document comprehensive sdk design blueprint and abstractions` |
| **Phase 7** | Limits, Error Catalog & Storage Benchmarks | Subagent `ref-analyst` (Gemini Flash) | `06-reference-and-benchmarks/*.md` | `docs(research): add limits, complete error catalog, and storage comparison` |
| **Phase 8** | Master Architectural Synthesis & Key Learnings | Parent Orchestrator + Subagent Synthesis | `README.md`, `WHAT_WE_LEARN.md` | `docs(research): add master readme and what we learn synthesis` |

---

## 5. Execution Rules & Quality Standards

1. **Strictly Non-Hallucinatory**: Every assertion, parameter name, hex selector, error name, and data limit must directly cite the scraped documentation.
2. **Exhaustive Code & Calldata Examples**: All endpoints and EVM structs must include concrete hexadecimal calldata layouts, JSON payloads, and TypeScript/Go/Rust pseudo-code.
3. **Mermaid Diagrams**: Every major architectural section must feature descriptive Mermaid sequence and architecture diagrams (flowcharts, state machines, and sequence diagrams).
4. **Git Discipline**: Every phase must conclude with an atomic commit on `research/arkiv-docs`. Never push to remotes.
