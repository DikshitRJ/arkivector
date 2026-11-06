"""Unit tests for Phase S5: LangChain and LlamaIndex vector store adapters."""

from __future__ import annotations

import sys
import types
from typing import Any, Dict, List, Optional
from unittest.mock import MagicMock

import pytest

from arkivector.filter import F
from arkivector.langchain import ArkivectorVectorStore as LCVstore
from arkivector.llama_index import (
    ArkivectorVectorStore as LIVstore,
)
from arkivector.llama_index import (
    _translate_filters,
    _translate_single_filter,
)
from arkivector.models import SearchHit, SearchResult, UpsertItemResult, UpsertResult

# ============================================================================
# 1. Lazy import error tests (when dependencies are missing)
# ============================================================================


def test_langchain_import_error_on_instantiation(monkeypatch: pytest.MonkeyPatch) -> None:
    """ArkivectorVectorStore for LangChain raises informative error if core missing."""
    for mod in list(sys.modules.keys()):
        if mod.startswith("langchain_core"):
            monkeypatch.delitem(sys.modules, mod, raising=False)

    import builtins

    real_import = builtins.__import__

    def mock_import(name: str, *args: Any, **kwargs: Any) -> Any:
        if name.startswith("langchain_core"):
            raise ImportError(f"No module named {name}")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", mock_import)

    with pytest.raises(ImportError) as exc_info:
        LCVstore(client=None, collection="test", embedding=None)

    assert "langchain-core is required to use ArkivectorVectorStore" in str(exc_info.value)
    assert "pip install" in str(exc_info.value)


def test_llama_index_import_error_on_instantiation(monkeypatch: pytest.MonkeyPatch) -> None:
    """ArkivectorVectorStore for LlamaIndex raises informative error if core missing."""
    for mod in list(sys.modules.keys()):
        if mod.startswith("llama_index"):
            monkeypatch.delitem(sys.modules, mod, raising=False)

    import builtins

    real_import = builtins.__import__

    def mock_import(name: str, *args: Any, **kwargs: Any) -> Any:
        if name.startswith("llama_index"):
            raise ImportError(f"No module named {name}")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", mock_import)

    with pytest.raises(ImportError) as exc_info:
        LIVstore(client=None, collection="test")

    assert "llama-index-core is required to use ArkivectorVectorStore with LlamaIndex" in str(
        exc_info.value
    )
    assert "pip install" in str(exc_info.value)


# ============================================================================
# 2. Mock fixtures for LangChain and LlamaIndex
# ============================================================================


@pytest.fixture
def mock_langchain_core(monkeypatch: pytest.MonkeyPatch) -> None:
    """Inject mock langchain_core into sys.modules."""
    lc_core = types.ModuleType("langchain_core")
    lc_docs = types.ModuleType("langchain_core.documents")
    lc_embed = types.ModuleType("langchain_core.embeddings")
    lc_vstores = types.ModuleType("langchain_core.vectorstores")

    class Document:
        def __init__(self, page_content: str, metadata: Optional[Dict[str, Any]] = None) -> None:
            self.page_content = page_content
            self.metadata = metadata or {}

    class Embeddings:
        pass

    class VectorStore:
        pass

    lc_docs.Document = Document  # type: ignore[attr-defined]
    lc_embed.Embeddings = Embeddings  # type: ignore[attr-defined]
    lc_vstores.VectorStore = VectorStore  # type: ignore[attr-defined]

    monkeypatch.setitem(sys.modules, "langchain_core", lc_core)
    monkeypatch.setitem(sys.modules, "langchain_core.documents", lc_docs)
    monkeypatch.setitem(sys.modules, "langchain_core.embeddings", lc_embed)
    monkeypatch.setitem(sys.modules, "langchain_core.vectorstores", lc_vstores)


@pytest.fixture
def mock_llama_index_core(monkeypatch: pytest.MonkeyPatch) -> None:
    """Inject mock llama_index.core into sys.modules."""
    li_core = types.ModuleType("llama_index.core")
    li_schema = types.ModuleType("llama_index.core.schema")
    li_vstores = types.ModuleType("llama_index.core.vector_stores")
    li_vstore_types = types.ModuleType("llama_index.core.vector_stores.types")

    class TextNode:
        def __init__(
            self,
            text: str = "",
            id_: Optional[str] = None,
            metadata: Optional[Dict[str, Any]] = None,
            embedding: Optional[List[float]] = None,
        ) -> None:
            self.text = text
            self.id_ = id_ or ""
            self.node_id = self.id_
            self.metadata = metadata or {}
            self.embedding = embedding

        def get_content(self) -> str:
            return self.text

        def get_embedding(self) -> Optional[List[float]]:
            return self.embedding

    class BasePydanticVectorStore:
        pass

    class VectorStoreQueryResult:
        def __init__(
            self,
            nodes: Optional[List[Any]] = None,
            similarities: Optional[List[float]] = None,
            ids: Optional[List[str]] = None,
        ) -> None:
            self.nodes = nodes or []
            self.similarities = similarities or []
            self.ids = ids or []

    li_schema.TextNode = TextNode  # type: ignore[attr-defined]
    li_vstore_types.BasePydanticVectorStore = BasePydanticVectorStore  # type: ignore[attr-defined]
    li_vstore_types.VectorStoreQueryResult = VectorStoreQueryResult  # type: ignore[attr-defined]

    monkeypatch.setitem(sys.modules, "llama_index", types.ModuleType("llama_index"))
    monkeypatch.setitem(sys.modules, "llama_index.core", li_core)
    monkeypatch.setitem(sys.modules, "llama_index.core.schema", li_schema)
    monkeypatch.setitem(sys.modules, "llama_index.core.vector_stores", li_vstores)
    monkeypatch.setitem(sys.modules, "llama_index.core.vector_stores.types", li_vstore_types)


# ============================================================================
# 3. LangChain VectorStore method tests
# ============================================================================


def test_langchain_vectorstore_full_lifecycle(mock_langchain_core: None) -> None:
    """Test LangChain vectorstore add_texts, similarity_search, delete, from_texts."""
    mock_client = MagicMock()
    mock_embed = MagicMock()
    mock_embed.embed_documents.return_value = [[0.1, 0.2], [0.3, 0.4]]
    mock_embed.embed_query.return_value = [0.1, 0.2]

    # 1. from_texts
    mock_client.upsert.return_value = UpsertResult(
        results=[
            UpsertItemResult(id="doc-1", key="0x1", op="create"),
            UpsertItemResult(id="doc-2", key="0x2", op="create"),
        ],
        tx_hashes=["0xtx"],
        block_number=100,
    )

    store = LCVstore.from_texts(
        texts=["First doc", "Second doc"],
        embedding=mock_embed,
        metadatas=[{"category": "finance"}, {"category": "tech"}],
        client=mock_client,
        collection="demo.docs",
        ids=["doc-1", "doc-2"],
    )

    assert store.collection == "demo.docs"
    assert mock_client.upsert.called
    upsert_args = mock_client.upsert.call_args[0]
    assert upsert_args[0] == "demo.docs"
    assert len(upsert_args[1]) == 2
    assert upsert_args[1][0]["id"] == "doc-1"
    assert upsert_args[1][0]["document"] == "First doc"
    assert upsert_args[1][0]["attributes"] == {"category": "finance"}

    # 2. similarity_search_with_score
    mock_hit = SearchHit.from_json(
        {
            "key": "0xabc",
            "id": "doc-1",
            "distance": 0.15,
            "score": 0.85,
            "attributes": {"category": "finance", "score": {"type": "dec", "value": "4.2"}},
            "document": "First doc",
        }
    )
    mock_client.search.return_value = SearchResult(
        as_of_block=100,
        sync_state="live",
        open_collection=False,
        hits=[mock_hit],
    )

    docs_and_scores = store.similarity_search_with_score(
        query="finance performance",
        k=1,
        filter={"category": "finance"},
    )
    assert len(docs_and_scores) == 1
    doc, dist = docs_and_scores[0]
    assert doc.page_content == "First doc"
    assert doc.metadata["id"] == "doc-1"
    assert doc.metadata["key"] == "0xabc"
    assert doc.metadata["category"] == "finance"
    assert doc.metadata["score"] == "4.2"
    assert dist == 0.15  # scores = distance

    # Verify search call filter translation (dict-of-equals -> F.and_(*[F.eq(k, v)...]))
    search_kwargs = mock_client.search.call_args[1]
    assert search_kwargs["k"] == 1
    assert search_kwargs["vector"] == [0.1, 0.2]
    filter_arg = search_kwargs["filter"]
    assert isinstance(filter_arg, F)
    ast = filter_arg.to_dict()
    assert "and" in ast
    assert ast["and"][0]["eq"]["category"]["value"] == "finance"

    # 3. similarity_search
    docs = store.similarity_search("query text", k=1)
    assert len(docs) == 1
    assert docs[0].page_content == "First doc"

    # 4. similarity_search_by_vector
    vec_docs = store.similarity_search_by_vector([0.1, 0.2], k=1)
    assert len(vec_docs) == 1

    # 5. delete
    mock_client.delete.return_value = None
    res = store.delete(ids=["doc-1", "doc-2"])
    assert res is True
    mock_client.delete.assert_called_with("demo.docs", refs=["doc-1", "doc-2"])


# ============================================================================
# 4. LlamaIndex MetadataFilters translation tests
# ============================================================================


class DummyFilter:
    def __init__(self, key: str, value: Any, operator: Any) -> None:
        self.key = key
        self.value = value
        self.operator = operator


class DummyFilters:
    def __init__(self, filters: List[Any], condition: str = "and") -> None:
        self.filters = filters
        self.condition = condition


def test_llama_index_filter_translation_all_operators() -> None:
    """Test translating EQ, NE, GT, GTE, LT, LTE, IN, NIN to F nodes."""
    # EQ
    f_eq = _translate_single_filter(DummyFilter("status", "active", "=="))
    assert f_eq is not None
    assert f_eq.to_dict() == {"eq": {"status": {"type": "str", "value": "active"}}}

    # NE
    f_ne = _translate_single_filter(DummyFilter("status", "archived", "!="))
    assert f_ne is not None
    assert f_ne.to_dict() == {"ne": {"status": {"type": "str", "value": "archived"}}}

    # GT with float -> dec
    f_gt = _translate_single_filter(DummyFilter("score", 4.0, ">"))
    assert f_gt is not None
    assert f_gt.to_dict() == {"gt": {"score": {"type": "dec", "value": "4.0"}}}

    # GTE with int -> i32
    f_gte = _translate_single_filter(DummyFilter("rank", 10, ">="))
    assert f_gte is not None
    assert f_gte.to_dict() == {"gte": {"rank": {"type": "i32", "value": 10}}}

    # LT
    f_lt = _translate_single_filter(DummyFilter("rank", 100, "<"))
    assert f_lt is not None
    assert f_lt.to_dict() == {"lt": {"rank": {"type": "i32", "value": 100}}}

    # LTE
    f_lte = _translate_single_filter(DummyFilter("rank", 50, "<="))
    assert f_lte is not None
    assert f_lte.to_dict() == {"lte": {"rank": {"type": "i32", "value": 50}}}

    # IN (list of values -> OR of EQ)
    f_in = _translate_single_filter(DummyFilter("category", ["finance", "tech"], "in"))
    assert f_in is not None
    ast_in = f_in.to_dict()
    assert "or" in ast_in
    assert len(ast_in["or"]) == 2
    assert ast_in["or"][0]["eq"]["category"]["value"] == "finance"
    assert ast_in["or"][1]["eq"]["category"]["value"] == "tech"

    # NIN (not of OR of EQ)
    f_nin = _translate_single_filter(DummyFilter("category", ["sports"], "nin"))
    assert f_nin is not None
    ast_nin = f_nin.to_dict()
    assert "not" in ast_nin
    assert ast_nin["not"]["eq"]["category"]["value"] == "sports"

    # Composite MetadataFilters with OR
    comp = DummyFilters(
        filters=[
            DummyFilter("cat", "a", "eq"),
            DummyFilter("cat", "b", "eq"),
        ],
        condition="or",
    )
    f_comp = _translate_filters(comp)
    assert f_comp is not None
    ast_comp = f_comp.to_dict()
    assert "or" in ast_comp
    assert len(ast_comp["or"]) == 2


# ============================================================================
# 5. LlamaIndex VectorStore method tests
# ============================================================================


def test_llama_index_vectorstore_methods(mock_llama_index_core: None) -> None:
    """Test LlamaIndex vectorstore add, delete, and query."""
    from llama_index.core.schema import TextNode  # type: ignore[import-not-found]

    mock_client = MagicMock()
    mock_client.upsert.return_value = UpsertResult(
        results=[UpsertItemResult(id="node-1", key="0x1", op="create")],
        tx_hashes=["0xtx"],
        block_number=100,
    )

    store = LIVstore(client=mock_client, collection="demo.docs")
    assert store.collection == "demo.docs"
    assert store.client == mock_client

    # 1. add
    node = TextNode(
        text="A sample node document",
        id_="node-1",
        metadata={"category": "finance", "score": "4.5"},
        embedding=[0.1, 0.2, 0.3],
    )
    added_ids = store.add([node])
    assert added_ids == ["node-1"]
    assert mock_client.upsert.called
    upsert_args = mock_client.upsert.call_args[0]
    assert upsert_args[1][0]["id"] == "node-1"
    assert upsert_args[1][0]["document"] == "A sample node document"
    assert upsert_args[1][0]["vector"] == [0.1, 0.2, 0.3]

    # 2. delete
    store.delete(ref_doc_id="node-1")
    mock_client.delete.assert_called_with("demo.docs", refs=["node-1"])

    # 3. query
    mock_hit = SearchHit.from_json(
        {
            "key": "0xabc",
            "id": "node-1",
            "distance": 0.12,
            "score": 0.88,
            "attributes": {"category": "finance"},
            "document": "A sample node document",
        }
    )
    mock_client.search.return_value = SearchResult(
        as_of_block=100,
        sync_state="live",
        open_collection=False,
        hits=[mock_hit],
    )

    query_obj = types.SimpleNamespace(
        query_embedding=[0.1, 0.2, 0.3],
        similarity_top_k=2,
        filters=DummyFilters([DummyFilter("category", "finance", "eq")]),
        node_ids=None,
    )
    result = store.query(query_obj)
    assert len(result.nodes) == 1
    assert result.nodes[0].text == "A sample node document"
    assert result.nodes[0].node_id == "node-1"
    assert result.similarities == [0.88]
    assert result.ids == ["node-1"]
