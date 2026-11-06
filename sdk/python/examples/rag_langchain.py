"""Self-contained RAG demo using Arkivector with LangChain VectorStore adapter."""

from __future__ import annotations

import hashlib
import math
import os
import sys
import types
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Ensure sdk/python/src is in sys.path
SDK_SRC = Path(__file__).resolve().parent.parent / "src"
if str(SDK_SRC) not in sys.path:
    sys.path.insert(0, str(SDK_SRC))

# Self-contained shim for langchain_core if not installed in the environment
try:
    import langchain_core  # noqa: F401
    import langchain_core.documents  # noqa: F401
    import langchain_core.embeddings  # noqa: F401
    import langchain_core.vectorstores  # noqa: F401
except ImportError:
    lc_core = types.ModuleType("langchain_core")
    lc_docs = types.ModuleType("langchain_core.documents")
    lc_embed = types.ModuleType("langchain_core.embeddings")
    lc_vstores = types.ModuleType("langchain_core.vectorstores")

    class _Document:
        """Lightweight Document shim."""

        def __init__(
            self,
            page_content: str,
            metadata: Optional[Dict[str, Any]] = None,
        ) -> None:
            self.page_content = page_content
            self.metadata = metadata or {}

        def __repr__(self) -> str:
            return f"Document(page_content={self.page_content!r}, metadata={self.metadata!r})"

    class _Embeddings:
        """Lightweight Embeddings base shim."""

        pass

    class _VectorStore:
        """Lightweight VectorStore base shim."""

        pass

    lc_docs.Document = _Document
    lc_embed.Embeddings = _Embeddings
    lc_vstores.VectorStore = _VectorStore

    sys.modules["langchain_core"] = lc_core
    sys.modules["langchain_core.documents"] = lc_docs
    sys.modules["langchain_core.embeddings"] = lc_embed
    sys.modules["langchain_core.vectorstores"] = lc_vstores

from arkivector import Client  # noqa: E402
from arkivector.langchain import ArkivectorVectorStore  # noqa: E402


class DeterministicEmbeddings:
    """Deterministic 8-dimensional embedder based on text SHA-256 hash."""

    def __init__(self, dim: int = 8) -> None:
        self.dim = dim

    def embed_text(self, text: str) -> List[float]:
        """Generate normalized 8-dim float vector deterministically from text."""
        h = hashlib.sha256(text.encode("utf-8")).digest()
        raw = []
        for i in range(self.dim):
            # Take 2 bytes per dimension and map to [-1.0, 1.0]
            val = ((h[2 * i] << 8) | h[2 * i + 1]) / 32767.5 - 1.0
            raw.append(val)
        norm = math.sqrt(sum(x * x for x in raw))
        if norm > 0:
            return [round(x / norm, 4) for x in raw]
        return [0.0] * self.dim

    def embed_documents(self, texts: List[str]) -> List[List[float]]:
        """Embed list of document texts."""
        return [self.embed_text(t) for t in texts]

    def embed_query(self, text: str) -> List[float]:
        """Embed a single query text."""
        return self.embed_text(text)


def synthesize_answer(query: str, hits: List[Tuple[Any, float]]) -> str:
    """Synthesize an answer from retrieved document hits and metadata."""
    if not hits:
        return f"No relevant documents found for query: '{query}'."

    context_lines = []
    for idx, (doc, dist) in enumerate(hits, start=1):
        content = (
            doc.page_content
            if doc.page_content
            else f"[Entity {doc.metadata.get('id', doc.metadata.get('key', 'unknown'))}]"
        )
        meta_str = ", ".join(f"{k}={v}" for k, v in sorted(doc.metadata.items()))
        context_lines.append(f"  {idx}. {content} (distance: {dist:.4f}, metadata: {{{meta_str}}})")

    top_doc = hits[0][0]
    category = top_doc.metadata.get("category", "unspecified")
    score = top_doc.metadata.get("score", "N/A")
    entity_id = top_doc.metadata.get("id", top_doc.metadata.get("key", "unknown"))

    return (
        f"--- Synthesized Answer ---\n"
        f"Question: {query}\n\n"
        f"Retrieved Documents ({len(hits)}):\n" + "\n".join(context_lines) + f"\n\nSynthesis:\n"
        f"Based on the most relevant match ({entity_id}), category is '{category}' "
        f"with score '{score}' at vector distance {hits[0][1]:.4f}.\n"
        f"--------------------------"
    )


def main() -> None:
    url = os.environ.get("ARKIVECTOR_URL", "http://127.0.0.1:4010")
    print(f"Connecting to Arkivector daemon at {url}...")
    client = Client(url=url)

    v = client.version()
    print(f"Daemon version: {v.daemon_version} (API {v.api_version})")

    # When running against Prism mock, OpenAPI /v1/version example omits entities.write
    if not client.has_capability("entities.write"):
        v.capabilities.extend(["entities.write", "entities.prepare"])

    collection_name = "demo.docs"
    embedder = DeterministicEmbeddings(dim=8)

    print(f"Initializing ArkivectorVectorStore on collection '{collection_name}'...")
    store = ArkivectorVectorStore(
        client=client,
        collection=collection_name,
        embedding=embedder,
    )

    # 1. Ingestion: Add sample texts to vectorstore
    print("\n[Step 1] Adding sample documents via store.add_texts()...")
    sample_texts = [
        "Q3 earnings increased by 14% with strong growth in enterprise subscriptions.",
        "Operational efficiency improved by 5% following warehouse logistics optimization.",
    ]
    sample_metas = [
        {"category": "finance", "score": "4.8"},
        {"category": "logistics", "score": "3.5"},
    ]
    sample_ids = ["doc-rag-001", "doc-rag-002"]

    added_ids = store.add_texts(
        texts=sample_texts,
        metadatas=sample_metas,
        ids=sample_ids,
    )
    print(f"Successfully indexed documents with IDs: {added_ids}")

    # 2. Similarity search with score
    query = "What is the financial earnings growth?"
    print(f"\n[Step 2] Executing similarity search for: '{query}'...")
    hits = store.similarity_search_with_score(query=query, k=2)
    print(f"Retrieved {len(hits)} hits:")
    for doc, dist in hits:
        print(f"  - Document ID={doc.metadata.get('id')}: distance={dist:.4f}")

    # 3. Filtered search
    print("\n[Step 3] Executing filtered search with filter={'category': 'finance'}...")
    filtered_hits = store.similarity_search_with_score(
        query=query,
        k=2,
        filter={"category": "finance"},
    )
    print(f"Retrieved {len(filtered_hits)} filtered hits:")
    for doc, dist in filtered_hits:
        cat = doc.metadata.get("category")
        print(f"  - Document ID={doc.metadata.get('id')}, category={cat}: distance={dist:.4f}")

    # 4. Search by vector
    print("\n[Step 4] Executing similarity_search_by_vector...")
    query_vector = embedder.embed_query(query)
    vector_docs = store.similarity_search_by_vector(query_vector, k=2)
    print(f"Retrieved {len(vector_docs)} documents via vector query.")

    # 5. Answer synthesis
    print("\n[Step 5] Answer Synthesis from retrieved context...")
    synthesis = synthesize_answer(query, hits)
    print(synthesis)

    print("\nRAG LangChain demo completed successfully!")


if __name__ == "__main__":
    main()
