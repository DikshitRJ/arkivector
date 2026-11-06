# pgvector-python: Architectural Analysis of Real-World Examples

> **Document Version:** 1.0.0  
> **Target SDK:** `pgvector-python` (`pgvector.psycopg`, `pgvector.psycopg2`)  
> **Source Directory Inspected:** `pgvector-python/examples/`  
> **Examples Analyzed:**  
> 1. `examples/rag/example.py`  
> 2. `examples/openai/example.py`  
> 3. `examples/openai/halfvec.py`  
> 4. `examples/hybrid_search/rrf.py`  
> 5. `examples/hybrid_search/cross_encoder.py`  
> 6. `examples/colbert/exact.py`  
> 7. `examples/colbert/approximate.py`  
> 8. `examples/colpali/exact.py`  
> 9. `examples/sparse_search/example.py`  
> 10. `examples/loading/example.py`  
> 11. `examples/image_search/example.py`  
> 12. `examples/sentence_transformers/example.py`  
> 13. `examples/cohere/example.py`  
> 14. `examples/citus/example.py`  

---

## Table of Contents

1. [Executive Summary & Taxonomy of Examples](#1-executive-summary--taxonomy-of-examples)
2. [Category 1: Retrieval-Augmented Generation (RAG)](#2-category-1-retrieval-augmented-generation-rag)
   - [2.1 Architecture of `rag/example.py`](#21-architecture-of-ragexamplepy)
   - [2.2 Ingestion & Psycopg Binary COPY Pipeline](#22-ingestion--psycopg-binary-copy-pipeline)
   - [2.3 Asymmetric Task Instruction Prefixes](#23-asymmetric-task-instruction-prefixes)
   - [2.4 Generation Integration via Local LLM (Ollama)](#24-generation-integration-via-local-llm-ollama)
3. [Category 2: Commercial Dense Embeddings (OpenAI)](#3-category-2-commercial-dense-embeddings-openai)
   - [3.1 Standard Dense Retrieval (`openai/example.py`)](#31-standard-dense-retrieval-openaiexamplepy)
   - [3.2 Half-Precision Optimization (`openai/halfvec.py`)](#32-half-precision-optimization-openaihalfvecpy)
   - [3.3 50% RAM/Disk Reduction via `halfvec(3072)`](#33-50-ramdisk-reduction-via-halfvec3072)
4. [Category 3: Hybrid Search (Lexical + Semantic)](#4-category-3-hybrid-search-lexical--semantic)
   - [4.1 Reciprocal Rank Fusion (`hybrid_search/rrf.py`)](#41-reciprocal-rank-fusion-hybrid_searchrrfpy)
   - [4.2 Cross-Encoder Re-Ranking (`hybrid_search/cross_encoder.py`)](#42-cross-encoder-re-ranking-hybrid_searchcross_encoderpy)
   - [4.3 Architectural Comparison: RRF vs Cross-Encoder](#43-architectural-comparison-rrf-vs-cross-encoder)
5. [Category 4: ColBERT Multi-Vector Late Interaction](#5-category-4-colbert-multi-vector-late-interaction)
   - [5.1 Multi-Vector Foundations: Why Single-Vector Pooling Fails](#51-multi-vector-foundations-why-single-vector-pooling-fails)
   - [5.2 Exact Multi-Vector Search with In-Database MaxSim (`colbert/exact.py`)](#52-exact-multi-vector-search-with-in-database-maxsim-colbertexactpy)
   - [5.3 Two-Stage Approximate Candidate Generation & Reranking (`colbert/approximate.py`)](#53-two-stage-approximate-candidate-generation--reranking-colbertapproximatepy)
6. [Category 5: Vision-Language Models & Visual RAG (ColPali)](#6-category-5-vision-language-models--visual-rag-colpali)
   - [5.1 Eliminating OCR: Page-Level Image Embeddings (`colpali/exact.py`)](#61-eliminating-ocr-page-level-image-embeddings-colpaliexactpy)
   - [5.2 Binary Quantization (`Bit(embedding > 0)`)](#62-binary-quantization-bitembedding--0)
   - [5.3 Hamming Distance MaxSim Operator in SQL](#63-hamming-distance-maxsim-operator-in-sql)
7. [Category 6: Sparse Semantic Search (SPLADE / Masked LM)](#7-category-6-sparse-semantic-search-splade--masked-lm)
   - [7.1 Neural Sparse Encoding Principles (`sparse_search/example.py`)](#71-neural-sparse-encoding-principles-sparse_searchexamplepy)
   - [7.2 Vocab-Dimensional Vectors: `sparsevec(30522)`](#72-vocab-dimensional-vectors-sparsevec30522)
   - [7.3 Max Inner Product `<#>` as Dot-Product Term Matching](#73-max-inner-product--as-dot-product-term-matching)
8. [Category 7: Computer Vision & Reverse Image Search](#8-category-7-computer-vision--reverse-image-search)
   - [8.1 Deep Feature Extraction via ResNet18 (`image_search/example.py`)](#81-deep-feature-extraction-via-resnet18-image_searchexamplepy)
   - [8.2 Latent Space Navigation with Cosine Distance `<=>`](#82-latent-space-navigation-with-cosine-distance-)
9. [Category 8: Enterprise Bulk Loading & Performance Tuning](#9-category-8-enterprise-bulk-loading--performance-tuning)
   - [9.1 High-Throughput Binary Ingestion (`loading/example.py`)](#91-high-throughput-binary-ingestion-loadingexamplepy)
   - [9.2 The "Build Index After Ingestion" Principle](#92-the-build-index-after-ingestion-principle)
   - [9.3 Parallel Index Build Parameters: `maintenance_work_mem` & Workers](#93-parallel-index-build-parameters-maintenance_work_mem--workers)
10. [Category 9: Distributed Scale-Out via Citus](#10-category-9-distributed-scale-out-via-citus)
    - [10.1 Sharding pgvector Across Worker Nodes (`citus/example.py`)](#101-sharding-pgvector-across-worker-nodes-citusexamplepy)
    - [10.2 Distributed Table Creation & Shard Key Selection](#102-distributed-table-creation--shard-key-selection)
    - [10.3 The Citus GUC Variable Propagation Trap](#103-the-citus-guc-variable-propagation-trap)
    - [10.4 Parallel Distributed Index Builds & Scatter-Gather KNN](#104-parallel-distributed-index-builds--scatter-gather-knn)
11. [Category 10: Compact Representations & Baseline Models](#11-category-10-compact-representations--baseline-models)
    - [11.1 Local SentenceTransformers (`sentence_transformers/example.py`)](#111-local-sentencetransformers-sentence_transformersexamplepy)
    - [11.2 32x Memory Reduction with Cohere Unsigned Binary (`cohere/example.py`)](#112-32x-memory-reduction-with-cohere-unsigned-binary-cohereexamplepy)
12. [Cross-Example Synthesis Matrix](#12-cross-example-synthesis-matrix)

---

## 1. Executive Summary & Taxonomy of Examples

The official `pgvector-python` examples demonstrate how PostgreSQL and `pgvector` function as an enterprise-grade vector database across real-world workloads. Rather than limiting vector search to simple toy embeddings, the example suite covers modern AI search paradigms:

```
                                  +----------------------------------------------------+
                                  |            pgvector Real-World Workloads           |
                                  +----------------------------------------------------+
                                                             |
            +--------------------+---------------------------+------------------------+--------------------+
            |                    |                           |                        |                    |
            v                    v                           v                        v                    v
     [Standard Dense]    [Precision / Bits]           [Multi-Vector]               [Hybrid]           [Enterprise]
     - OpenAI            - halfvec (3072d)            - ColBERT (Exact)            - SQL RRF          - Binary COPY
     - SentenceTransf.   - Cohere ubinary (2048-bit)  - ColBERT (Approx HNSW)      - CrossEncoder     - Citus Sharding
     - ResNet18 Images   - Sparsevec SPLADE (30522d)  - ColPali Binary Vision VLM                     - Parallel Index
```

---

## 2. Category 1: Retrieval-Augmented Generation (RAG)

### 2.1 Architecture of `rag/example.py`

`examples/rag/example.py` demonstrates a complete, local, open-source RAG pipeline that operates completely offline using Ollama and PostgreSQL:

```
[Markdown Doc] ──> [Chunking] ──> [Nomic Embed (768d)] ──> [Binary COPY] ──> [PostgreSQL]
                                                                                     │
[User Query]   ──> [Nomic Query Embed] ───────────────────> [<=> Cosine KNN] ────────┘
                                                                    │
                                                            [Top 5 Chunks]
                                                                    │
                                                            [Prompt Assembly]
                                                                    │
                                                          [Llama 3.2 Generator] ──> [Answer]
```

### 2.2 Ingestion & Psycopg Binary COPY Pipeline

Standard ORM `INSERT` statements incur substantial parsing overhead for large document corpora. The RAG example utilizes `psycopg`'s native streaming binary `COPY` protocol:

```python
cur = conn.cursor()
with cur.copy('COPY chunks (content, embedding) FROM STDIN WITH (FORMAT BINARY)') as copy:
    copy.set_types(['text', 'vector'])
    for content, embedding in zip(chunks, embeddings):
        copy.write_row([content, embedding])
```

- **`FORMAT BINARY`**: Transfers raw bytes directly into PostgreSQL’s tuple memory buffer without text quoting or string parsing.
- **`copy.set_types(['text', 'vector'])`**: Tells `psycopg` to use its C-level binary encoders for the `vector` type.

### 2.3 Asymmetric Task Instruction Prefixes

The example highlights a critical modern embedding practice: **asymmetric task prefixes**:
- **Documents:** `input = ['search_document: ' + chunk for chunk in chunks]`
- **Queries:** `input = f'search_query: {query}'`

Asymmetric embedding models (such as `nomic-embed-text`, E5, and BGE) train distinct projection layers for documents vs queries. Omitting these prefixes causes a severe drop in retrieval accuracy.

### 2.4 Generation Integration via Local LLM (Ollama)

Top chunks are retrieved via Cosine Distance (`<=>`), concatenated into a context block, and piped to `llama3.2`:

```python
result = conn.execute(
    'SELECT content FROM chunks ORDER BY embedding <=> %s LIMIT 5',
    (Vector(embedding),)
).fetchall()

context = '\n\n'.join([row[0] for row in result])
prompt = f'Answer this question: {query}\n\n{context}'
response = ollama.generate(model='llama3.2', prompt=prompt).response
```

---

## 3. Category 2: Commercial Dense Embeddings (OpenAI)

### 3.1 Standard Dense Retrieval (`openai/example.py`)

- **Model:** `text-embedding-3-small` (OpenAI)
- **Dimension:** 1536 float32 dimensions
- **Table Definition:** `CREATE TABLE documents (id bigserial PRIMARY KEY, content text, embedding vector(1536))`
- **Query Operator:** `<=>` (Cosine Distance)

```python
result = conn.execute(
    'SELECT content FROM documents ORDER BY embedding <=> %s LIMIT 5',
    (Vector(query_embedding),)
).fetchall()
```

### 3.2 Half-Precision Optimization (`openai/halfvec.py`)

OpenAI's premium model (`text-embedding-3-large`) outputs 3072 dimensions. At 32-bit float precision, 10 million vectors consume over 122 GB of RAM just for raw vector data, exceeding the memory bounds of standard servers.

`openai/halfvec.py` solves this using pgvector's `halfvec` type:

```sql
CREATE TABLE documents (
    id bigserial PRIMARY KEY, 
    content text, 
    embedding halfvec(3072)
);
CREATE INDEX ON documents USING hnsw (embedding halfvec_cosine_ops);
```

### 3.3 50% RAM/Disk Reduction via `halfvec(3072)`

- **Storage Comparison:**
  - `vector(3072)` = $3072 \times 4 \text{ bytes} = 12,288 \text{ bytes/row}$
  - `halfvec(3072)` = $3072 \times 2 \text{ bytes} = 6,144 \text{ bytes/row}$ (50% reduction)
- **Python Integration:** Passed as `HalfVector(embedding)`. Python uses an internal `array('H')` (unsigned 16-bit integers) packed via IEEE 754 half-precision float format (`struct.pack(f'{dim}e', *value)`), ensuring minimal memory overhead on the client.
- **Accuracy Tradeoff:** Studies show half-precision floating-point degradation is under 0.001 MTEB score points while doubling cache hit rates.

---

## 4. Category 3: Hybrid Search (Lexical + Semantic)

Pure vector search struggles with keyword precision (serial numbers, product codes, specific names), while pure full-text search struggles with conceptual synonyms and semantic meaning. The example repository demonstrates two distinct hybrid architectures.

### 4.1 Reciprocal Rank Fusion (`hybrid_search/rrf.py`)

`rrf.py` implements Reciprocal Rank Fusion (RRF) **purely inside PostgreSQL** using a Common Table Expression (CTE) and window functions.

```sql
WITH semantic_search AS (
    SELECT id, RANK () OVER (ORDER BY embedding <=> %(embedding)s) AS rank
    FROM documents
    ORDER BY embedding <=> %(embedding)s
    LIMIT 20
),
keyword_search AS (
    SELECT id, RANK () OVER (ORDER BY ts_rank_cd(to_tsvector('english', content), query) DESC)
    FROM documents, plainto_tsquery('english', %(query)s) query
    WHERE to_tsvector('english', content) @@ query
    ORDER BY ts_rank_cd(to_tsvector('english', content), query) DESC
    LIMIT 20
)
SELECT
    COALESCE(semantic_search.id, keyword_search.id) AS id,
    COALESCE(1.0 / (%(k)s + semantic_search.rank), 0.0) +
    COALESCE(1.0 / (%(k)s + keyword_search.rank), 0.0) AS score
FROM semantic_search
FULL OUTER JOIN keyword_search ON semantic_search.id = keyword_search.id
ORDER BY score DESC
LIMIT 5;
```

#### Key Architectural Details:
1. **No Score Calibration Required:** Traditional score blending requires normalizing cosine distance $[0, 2]$ and BM25 $[0, \infty)$. RRF bypasses score calibration by merging **ordinal ranks**:
   $$\text{RRF Score} = \frac{1}{k + \text{rank}_{\text{semantic}}} + \frac{1}{k + \text{rank}_{\text{keyword}}}$$
2. **Smoothing Constant ($k=60$):** Prevents top-ranked outlier results in one modality from dominating the other.
3. **`FULL OUTER JOIN`**: Guarantees documents found *only* by keywords or *only* by vector search are retained.
4. **Zero Client Latency:** Entire fusion occurs in PostgreSQL memory in a single database roundtrip.

### 4.2 Cross-Encoder Re-Ranking (`hybrid_search/cross_encoder.py`)

`cross_encoder.py` implements a two-stage retrieve-and-rerank architecture:

```python
# Stage 1: Async concurrent retrieval of candidates
results = await asyncio.gather(
    semantic_search(conn, query),
    keyword_search(conn, query)
)

# Stage 2: Deduplication
candidates = set(itertools.chain(*results))

# Stage 3: Full cross-attention reranking
encoder = CrossEncoder('cross-encoder/ms-marco-MiniLM-L-6-v2')
scores = encoder.predict([(query, item[1]) for item in candidates])
final_ranked = [v for _, v in sorted(zip(scores, candidates), reverse=True)]
```

- **Candidate Generation:** Runs semantic and keyword searches asynchronously and concurrently using `asyncio.gather`.
- **Cross-Encoder Model:** Passes query and candidate text simultaneously into a joint Transformer (`ms-marco-MiniLM-L-6-v2`), evaluating full cross-attention across all token pairs.
- **Superior Quality:** Cross-encoders capture intricate token relationships that bi-encoders lose during dot-product pooling.

### 4.3 Architectural Comparison: RRF vs Cross-Encoder

| Dimension | SQL RRF (`rrf.py`) | Cross-Encoder (`cross_encoder.py`) |
|---|---|---|
| **Execution Site** | 100% inside PostgreSQL | Distributed (PostgreSQL + Python Worker) |
| **Network Roundtrips** | 1 SQL query | 1 concurrent SQL query + Python inference |
| **Compute Overhead** | Negligible CPU (sorting integers) | High CPU/GPU (Transformer forward pass) |
| **Latency** | < 15 ms | 50 – 250 ms |
| **Re-Ranking Capacity** | Top 100+ candidates | Top 10 – 30 candidates max |
| **Scoring Quality** | High (Robust heuristic) | State-of-the-art (Deep cross-attention) |

---

## 5. Category 4: ColBERT Multi-Vector Late Interaction

### 5.1 Multi-Vector Foundations: Why Single-Vector Pooling Fails

Standard bi-encoders compress an entire 500-word document into a single vector (e.g. 768 floats). This single vector often loses fine-grained nuances, numbers, and specific entities.

**ColBERT (Contextualized Late Interaction over BERT)** keeps an embedding for **every token** in the document ($N \times 128$) and every token in the query ($M \times 128$). The similarity is computed via the **MaxSim** operator: for each query token, take the maximum dot product across all document tokens, and sum them:

$$\operatorname{MaxSim}(Q, D) = \sum_{i \in Q} \max_{j \in D} \left( E_{q_i} \cdot E_{d_j} \right)$$

### 5.2 Exact Multi-Vector Search with In-Database MaxSim (`colbert/exact.py`)

`colbert/exact.py` implements the MaxSim operator directly in PostgreSQL PL/pgSQL using SQL functions and array unnesting:

```sql
CREATE TABLE documents (
    id bigserial PRIMARY KEY, 
    content text, 
    embeddings vector(128)[]  -- Array of vectors!
);

CREATE OR REPLACE FUNCTION max_sim(document vector[], query vector[]) 
RETURNS double precision AS $$
    WITH queries AS (
        SELECT row_number() OVER () AS query_number, * 
        FROM (SELECT unnest(query) AS query)
    ),
    documents AS (
        SELECT unnest(document) AS document
    ),
    similarities AS (
        SELECT query_number, 1 - (document <=> query) AS similarity 
        FROM queries CROSS JOIN documents
    ),
    max_similarities AS (
        SELECT MAX(similarity) AS max_similarity 
        FROM similarities GROUP BY query_number
    )
    SELECT SUM(max_similarity) FROM max_similarities
$$ LANGUAGE SQL;
```

#### Query Execution:
```python
result = conn.execute(
    'SELECT content, max_sim(embeddings, %s) AS max_sim '
    'FROM documents ORDER BY max_sim DESC LIMIT 5', 
    (query_embeddings,)
).fetchall()
```

This runs exact token-level late interaction without any external vector indexer!

### 5.3 Two-Stage Approximate Candidate Generation & Reranking (`colbert/approximate.py`)

Evaluating `max_sim` sequentially across millions of documents is computationally prohibitive ($O(N_{\text{docs}} \times |Q| \times |D|)$). 

`colbert/approximate.py` implements an approximate indexing pattern based on the ColBERT paper (Khattab & Zaharia, SIGIR 2020):

```
                                      [Query Tokens: q1, q2, ... qM]
                                                     │
                             ┌───────────────────────┴───────────────────────┐
                             ▼                                               ▼
               [Token q1 HNSW KNN (Top 5)]                      [Token qM HNSW KNN (Top 5)]
                             │                                               │
                             └───────────────────────┬───────────────────────┘
                                                     ▼
                                        [UNION ALL Candidate IDs]
                                                     │
                                                     ▼
                                            [DISTINCT document_ids]
                                                     │
                                                     ▼
                                          [Aggregate Token Arrays]
                                                     │
                                                     ▼
                                    [Evaluate exact max_sim(embeddings, Q)]
                                                     │
                                                     ▼
                                               [Top 10 Docs]
```

#### Database Schema:
```sql
CREATE TABLE documents (id bigserial PRIMARY KEY, content text);
CREATE TABLE document_embeddings (
    id bigserial PRIMARY KEY, 
    document_id bigint, 
    embedding vector(128)
);

CREATE INDEX ON document_embeddings (document_id);
CREATE INDEX ON document_embeddings USING hnsw (embedding vector_cosine_ops);
```

#### Approximate Search Query:
```python
# Stage 1: Union of top-5 nearest document IDs for EACH query token via HNSW
approximate_stage = ' UNION ALL '.join([
    '(SELECT document_id FROM document_embeddings ORDER BY embedding <=> %s LIMIT 5)' 
    for _ in query_embeddings
])

sql = f"""
WITH approximate_stage AS (
    {approximate_stage}
),
embeddings AS (
    SELECT document_id, array_agg(embedding) AS embeddings 
    FROM document_embeddings
    WHERE document_id IN (SELECT DISTINCT document_id FROM approximate_stage)
    GROUP BY document_id
)
SELECT content, max_sim(embeddings, %s) AS max_sim 
FROM documents
INNER JOIN embeddings ON embeddings.document_id = documents.id
ORDER BY max_sim DESC LIMIT 10
"""
```

This scales ColBERT multi-vector search to large collections by reducing full MaxSim scoring to only the top candidate documents retrieved through token-level HNSW indexes.

---

## 6. Category 5: Vision-Language Models & Visual RAG (ColPali)

### 6.1 Eliminating OCR: Page-Level Image Embeddings (`colpali/exact.py`)

Traditional RAG on PDFs and scanned documents requires complex OCR pipelines, layout parsing, and bounding-box extractors, which frequently corrupt tables, charts, and infographics.

`examples/colpali/exact.py` utilizes **ColPali** (`vidore/colqwen2-v1.0`), a Vision-Language Model (VLM) based on PaliGemma and Qwen2-VL:
- Ingests the **raw image of the document page directly**.
- Projects image patch tokens into ColBERT-style 128-dimensional multi-vector embeddings.
- Enables queries to search directly against visual layout, typography, charts, diagrams, and text simultaneously.

### 6.2 Binary Quantization (`Bit(embedding > 0)`)

Document pages contain up to 1024 patch tokens per page. Storing $1024 \times 128$ floats per page requires massive storage.

`colpali/exact.py` applies **Binary Quantization**:

```python
def binary_quantize(embedding):
    return Bit(embedding > 0)
```

- Each 128-dimensional float embedding is converted to a 128-bit string (`bit(128)`).
- If float $> 0 \rightarrow 1$, else $0$.
- **Storage reduction:** From 512 bytes ($128 \times 4$) down to **16 bytes** ($128 \text{ bits}$) per token—a **32x compression ratio**.

### 6.3 Hamming Distance MaxSim Operator in SQL

The example defines `max_sim` for `bit[]` arrays using the PostgreSQL bit Hamming distance operator `<~>`:

```sql
CREATE TABLE documents (id bigserial PRIMARY KEY, embeddings bit(128)[]);

CREATE OR REPLACE FUNCTION max_sim(document bit[], query bit[]) 
RETURNS double precision AS $$
    WITH queries AS (
        SELECT row_number() OVER () AS query_number, * 
        FROM (SELECT unnest(query) AS query)
    ),
    documents AS (
        SELECT unnest(document) AS document
    ),
    similarities AS (
        SELECT query_number, 
               1 - ((document <~> query) / bit_length(query)) AS similarity 
        FROM queries CROSS JOIN documents
    ),
    max_similarities AS (
        SELECT MAX(similarity) AS max_similarity 
        FROM similarities GROUP BY query_number
    )
    SELECT SUM(max_similarity) FROM max_similarities
$$ LANGUAGE SQL;
```

Hamming distance on bit strings executes in hardware via `POPCNT` (CPU population count) instructions, achieving blazing-fast visual search.

---

## 7. Category 6: Sparse Semantic Search (SPLADE / Masked LM)

### 7.1 Neural Sparse Encoding Principles (`sparse_search/example.py`)

Dense vectors encode general meaning into latent dimensions, but suffer from the "hubness" problem and lack exact keyword precision. 

`sparse_search/example.py` uses `opensearch-project/opensearch-neural-sparse-encoding-v1` (a SPLADE-class model):
- Projects document text into the **vocabulary space** of BERT (30,522 dimensions).
- Only non-zero activations (actual words and inferred semantic synonyms) are stored.

### 7.2 Vocab-Dimensional Vectors: `sparsevec(30522)`

```python
# Extraction of term activations across vocabulary
output = model(**feature)[0]
values, _ = torch.max(output * feature['attention_mask'].unsqueeze(-1), dim=1)
values = torch.log(1 + torch.relu(values))
values[:, special_token_ids] = 0
```

- **Database Type:** `sparsevec(30522)`
- Stored as indices and values: `"{101: 1.2, 2045: 0.8}/30522"`
- Only non-zero weights consume storage on disk.

### 7.3 Max Inner Product `<#>` as Dot-Product Term Matching

Because sparse vectors represent term weights across a shared vocabulary, document similarity is the dot product (sum of matching term weights):

```python
result = conn.execute(
    'SELECT content FROM documents ORDER BY embedding <#> %s LIMIT 5',
    (SparseVector(query_embedding),)
).fetchall()
```

Using `<#>` (negative inner product) leverages pgvector's sparse vector arithmetic to perform learned lexical search directly in PostgreSQL.

---

## 8. Category 7: Computer Vision & Reverse Image Search

### 8.1 Deep Feature Extraction via ResNet18 (`image_search/example.py`)

`examples/image_search/example.py` demonstrates content-based image retrieval (CBIR) on the CIFAR-10 dataset:
- Uses a pre-trained `torchvision.models.resnet18(weights='DEFAULT')`.
- Strips the final classification head:
  ```python
  model.fc = torch.nn.Identity()
  ```
- Extracts raw 512-dimensional continuous feature vectors representing visual semantics (textures, objects, colors, shapes).

### 8.2 Latent Space Navigation with Cosine Distance `<=>`

```sql
CREATE TABLE images (id bigserial PRIMARY KEY, embedding vector(512));
SELECT id FROM images ORDER BY embedding <=> %s LIMIT 5;
```

A query image (unseen by the model) is embedded and matched against 50,000 indexed images. The top-5 nearest neighbors accurately return visually and semantically similar objects.

---

## 9. Category 8: Enterprise Bulk Loading & Performance Tuning

### 9.1 High-Throughput Binary Ingestion (`loading/example.py`)

Ingesting millions of vectors via sequential `INSERT` statements can take hours due to round-trip latency, SQL parsing, and WAL amplification.

`examples/loading/example.py` provides the canonical high-throughput blueprint for 1,000,000 vectors:

```python
cur = conn.cursor()
with cur.copy('COPY items (embedding) FROM STDIN WITH (FORMAT BINARY)') as copy:
    copy.set_types(['vector'])
    for i, embedding in enumerate(embeddings):
        copy.write_row([Vector(embedding)])
```

Binary streaming achieves ingestion rates in excess of **50,000 to 100,000 vectors per second**.

### 9.2 The "Build Index After Ingestion" Principle

A common mistake in vector database deployments is creating an HNSW or IVFFlat index *before* bulk inserting rows:
- **Inserting into an existing HNSW index:** Each insertion requires traversing the graph, finding nearest neighbors, adjusting bidirectional links, and generating heavy WAL traffic ($O(N \log N)$ slow random I/O).
- **Golden Rule:** Drop indexes $\rightarrow$ Bulk load data $\rightarrow$ Build index in a single parallel sweep.

### 9.3 Parallel Index Build Parameters: `maintenance_work_mem` & Workers

`loading/example.py` documents the essential PostgreSQL GUC settings for fast index construction:

```sql
-- Allocate generous RAM for index building graph structures
SET maintenance_work_mem = '8GB';

-- Maximize multi-core parallelism for HNSW/IVFFlat builds
SET max_parallel_maintenance_workers = 7;

-- Build index
CREATE INDEX ON items USING hnsw (embedding vector_cosine_ops);

-- Update query planner statistics
ANALYZE items;
```

- **`maintenance_work_mem`**: Prevents spilling HNSW neighbor graphs to temporary disk files.
- **`max_parallel_maintenance_workers`**: Uses all CPU cores during index creation.
- **`ANALYZE`**: Ensures the PostgreSQL query planner knows accurate vector distributions.

---

## 10. Category 9: Distributed Scale-Out via Citus

### 10.1 Sharding pgvector Across Worker Nodes (`citus/example.py`)

When vector datasets grow beyond the memory capacity of a single physical server (e.g. 100M+ vectors requiring 500GB+ RAM), `pgvector` pairs with **Citus** (distributed PostgreSQL).

```
                            +-----------------------------------+
                            |     Citus Coordinator Node        |
                            |  - Client connections             |
                            |  - Scatter / Gather KNN Queries   |
                            +-----------------------------------+
                                              |
                   +--------------------------+--------------------------+
                   |                                                     |
                   v                                                     v
    +-----------------------------+                       +-----------------------------+
    |       Worker Node 1         |                       |       Worker Node 2         |
    |  - Shard 102008 (HNSW)      |                       |  - Shard 102010 (HNSW)      |
    |  - Shard 102009 (HNSW)      |                       |  - Shard 102011 (HNSW)      |
    +-----------------------------+                       +-----------------------------+
```

### 10.2 Distributed Table Creation & Shard Key Selection

```sql
CREATE EXTENSION citus;
CREATE EXTENSION vector;

CREATE TABLE items (
    id bigserial, 
    embedding vector(128), 
    category_id bigint, 
    PRIMARY KEY (id, category_id)
);

SET citus.shard_count = 4;
SELECT create_distributed_table('items', 'category_id');
```

- Citus shards data across worker nodes using the hash of `category_id`.
- Sharding allows vector data, index structures, and memory working sets to scale horizontally across dozens of machines.

### 10.3 The Citus GUC Variable Propagation Trap

`examples/citus/example.py` highlights a critical production gotcha:
> **PostgreSQL GUC variables set at the session level on the coordinator do NOT propagate to Citus worker nodes!**

If you run `SET hnsw.ef_search = 100;` on your connection, the worker nodes executing the shard queries will continue to use the default (`ef_search = 40`), causing lower recall!

#### Solution:
Persist GUC settings at the database or user level:
```sql
ALTER DATABASE pgvector_citus SET maintenance_work_mem = '512MB';
ALTER DATABASE pgvector_citus SET hnsw.ef_search = 20;
```
Then close and reconnect the connection.

### 10.4 Parallel Distributed Index Builds & Scatter-Gather KNN

- **Parallel Indexing:** `CREATE INDEX ON items USING hnsw (embedding vector_l2_ops);` executes concurrently across all worker shards.
- **Distributed Querying:**
  ```python
  items = conn.execute(
      'SELECT id FROM items ORDER BY embedding <-> %s LIMIT 10', 
      (query,)
  ).fetchall()
  ```
  The Citus coordinator fans out the nearest-neighbor query to all worker nodes in parallel, retrieves the top 10 from each shard, and performs a merge sort to return the global top 10.

---

## 11. Category 10: Compact Representations & Baseline Models

### 11.1 Local SentenceTransformers (`sentence_transformers/example.py`)

A lean, standard baseline for local dense semantic search:
- **Model:** `sentence-transformers/multi-qa-MiniLM-L6-cos-v1`
- **Dimensions:** 384 dimensions (float32)
- **Table Definition:** `CREATE TABLE documents (id bigserial PRIMARY KEY, content text, embedding vector(384))`
- **Characteristics:** Runs efficiently on CPU, suitable for lightweight semantic retrieval and testing.

### 11.2 32x Memory Reduction with Cohere Unsigned Binary (`cohere/example.py`)

`examples/cohere/example.py` showcases state-of-the-art embedding compression using Cohere's `embed-v5.0-pro` model with `embedding_types=['ubinary']`:

```python
co = cohere.ClientV2()
response = co.embed(
    texts=input, 
    model='embed-v5.0-pro', 
    input_type='search_document', 
    embedding_types=['ubinary']
)
```

- **Output:** Returns packed bytes representing 2048-bit binary vectors.
- **Storage:** `CREATE TABLE documents (id bigserial PRIMARY KEY, content text, embedding bit(2048))`
- **Query Operator:** `<~>` (Hamming distance).
- **Compression:** A 2048-dimensional float32 vector is 8,192 bytes. A 2048-bit binary vector is **256 bytes**. This allows 10 million vectors to fit inside 2.5 GB of RAM.

---

## 12. Cross-Example Synthesis Matrix

The table below synthesizes all 14 official examples across models, vector formats, metrics, and engineering innovations:

| Example File | Category | Embedding Model / Backbone | Dim & Type | Storage / Index | Operator & Metric | Core Innovation / Key Technique |
|---|---|---|---|---|---|---|
| `rag/example.py` | RAG | `nomic-embed-text` + `llama3.2` | 768 float32 | `vector(768)` (Heap) | `<=>` Cosine | Offline RAG, binary COPY, asymmetric task prefixes (`search_document: ` / `search_query: `) |
| `openai/example.py` | Dense Semantic | `text-embedding-3-small` | 1536 float32 | `vector(1536)` (Heap) | `<=>` Cosine | Direct commercial API integration |
| `openai/halfvec.py` | Dense Semantic | `text-embedding-3-large` | 3072 float16 | `halfvec(3072)` HNSW | `<=>` Cosine (`halfvec_cosine_ops`) | 50% RAM reduction on large models; half-precision HNSW index |
| `hybrid_search/rrf.py` | Hybrid | `multi-qa-MiniLM-L6-cos-v1` + Full-Text | 384 float32 | `vector(384)` + GIN `tsvector` | `<=>` Cosine + `@@` FullText | Pure in-database Reciprocal Rank Fusion via SQL CTE & window functions ($k=60$) |
| `hybrid_search/cross_encoder.py` | Hybrid | `multi-qa-MiniLM` + `ms-marco-MiniLM` | 384 float32 | `vector(384)` + GIN `tsvector` | `<=>` Cosine + `@@` FullText | Async concurrent retrieve (`asyncio.gather`), union deduplication, deep cross-attention reranking |
| `colbert/exact.py` | Multi-Vector | `colbert-ir/colbertv2.0` | $N \times 128$ float32 | `vector(128)[]` (Array) | In-database `max_sim` (PL/pgSQL) | Token-level multi-vector late interaction (MaxSim) directly in PostgreSQL |
| `colbert/approximate.py` | Multi-Vector | `colbert-ir/colbertv2.0` | 128 float32 | `vector(128)` HNSW + array aggregate | Token HNSW `<=>` + exact `max_sim` | Two-stage ColBERT: token-level candidate retrieve stage via HNSW + array aggregation & exact MaxSim rerank |
| `colpali/exact.py` | Vision VLM | `vidore/colqwen2-v1.0` (ColQwen2) | $N \times 128$ bits | `bit(128)[]` (Array) | `max_sim` over `<~>` (Hamming) | Direct page image embedding (no OCR), binary quantization ($>0 \rightarrow 1$), 32x compression, bit Hamming MaxSim |
| `sparse_search/example.py` | Learned Sparse | `opensearch-neural-sparse-encoding-v1` | 30522 sparse float | `sparsevec(30522)` (Heap) | `<#>` Inner Product | SPLADE-style term activations over BERT vocabulary; exact vocabulary dot product |
| `loading/example.py` | Enterprise Ingestion | Synthetic NumPy | 128 float32 | `vector(128)` HNSW | `<=>` Cosine | 1M row binary streaming `COPY`, build index *after* bulk load, `maintenance_work_mem = '8GB'`, parallel workers |
| `image_search/example.py` | Computer Vision | `torchvision.models.resnet18` | 512 float32 | `vector(512)` (Heap) | `<=>` Cosine | Reverse image search, latent feature extraction by stripping classification head (`Identity()`) |
| `sentence_transformers/example.py` | Dense Baseline | `multi-qa-MiniLM-L6-cos-v1` | 384 float32 | `vector(384)` (Heap) | `<=>` Cosine | Lean, local, CPU-friendly dense sentence embedding baseline |
| `cohere/example.py` | Binary Quantization | `embed-v5.0-pro` (Cohere) | 2048 bits | `bit(2048)` (Heap) | `<~>` Hamming | 32x memory savings via unsigned binary (`ubinary`), native packed byte ingestion via `Bit` |
| `citus/example.py` | Distributed Scale-Out | Synthetic NumPy | 128 float32 | Sharded `vector(128)` HNSW | `<->` L2 Euclidean | Horizontal scaling via Citus sharding, parallel distributed HNSW index build, worker GUC configuration rules |
