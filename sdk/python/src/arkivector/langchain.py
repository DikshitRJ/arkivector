"""LangChain VectorStore adapter for Arkivector."""

from __future__ import annotations

import uuid
from typing import (
    TYPE_CHECKING,
    Any,
    Dict,
    Iterable,
    List,
    Optional,
    Sequence,
    Tuple,
    Type,
    Union,
)

from arkivector.filter import F
from arkivector.models import T

if TYPE_CHECKING:
    from langchain_core.documents import Document
    from langchain_core.embeddings import Embeddings
    from langchain_core.vectorstores import VectorStore
else:
    try:
        from langchain_core.documents import Document
        from langchain_core.embeddings import Embeddings
        from langchain_core.vectorstores import VectorStore
    except ImportError:
        VectorStore = object
        Embeddings = object
        Document = object


def _get_langchain_core() -> Tuple[Type[Any], Type[Any], Type[Any]]:
    """Lazy import of langchain_core classes."""
    try:
        from langchain_core.documents import Document as LCDocument
        from langchain_core.embeddings import Embeddings as LCEmbeddings
        from langchain_core.vectorstores import VectorStore as LCVectorStore

        return LCVectorStore, LCEmbeddings, LCDocument
    except ImportError as exc:
        raise ImportError(
            "langchain-core is required to use ArkivectorVectorStore. "
            "Please install it with: pip install 'arkivector[langchain]' "
            "or pip install langchain-core"
        ) from exc


def _translate_filter(
    filter_arg: Optional[Union[F, Dict[str, Any]]],
) -> Optional[Union[F, Dict[str, Any]]]:
    """Translate dict-of-equals or preserve F AST node."""
    if filter_arg is None:
        return None
    if isinstance(filter_arg, F):
        return filter_arg
    if isinstance(filter_arg, dict):
        if not filter_arg:
            return None
        wire_ops = {
            "and",
            "or",
            "not",
            "eq",
            "ne",
            "lt",
            "lte",
            "gt",
            "gte",
            "between",
            "startswith",
            "exists",
            "typeof",
        }
        if len(filter_arg) == 1 and next(iter(filter_arg)) in wire_ops:
            return filter_arg
        children = [F.eq(k, T.dec(v) if isinstance(v, float) else v) for k, v in filter_arg.items()]
        return F.and_(*children)
    return filter_arg


class ArkivectorVectorStore(VectorStore):  # type: ignore[misc]
    """Arkivector VectorStore implementation for LangChain."""

    def __init__(
        self,
        client: Any,
        collection: str,
        embedding: Any,
        **kwargs: Any,
    ) -> None:
        """Initialize Arkivector VectorStore.

        Raises:
            ImportError: If langchain-core is not installed.
        """
        _, _, self._document_cls = _get_langchain_core()
        super().__init__()
        self.client = client
        self.collection = collection
        self.embedding = embedding

    def add_texts(
        self,
        texts: Iterable[str],
        metadatas: Optional[List[Dict[str, Any]]] = None,
        ids: Optional[List[str]] = None,
        **kwargs: Any,
    ) -> List[str]:
        """Run texts through embeddings and add to the collection."""
        text_list = list(texts)
        if not text_list:
            return []

        embeddings = self.embedding.embed_documents(text_list)
        entities: List[Dict[str, Any]] = []
        assigned_ids: List[str] = []

        for i, text in enumerate(text_list):
            ent_id = ids[i] if ids and i < len(ids) and ids[i] is not None else uuid.uuid4().hex
            meta = (
                metadatas[i]
                if metadatas and i < len(metadatas) and metadatas[i] is not None
                else {}
            )
            entities.append(
                {
                    "id": ent_id,
                    "vector": embeddings[i],
                    "document": text,
                    "attributes": dict(meta),
                }
            )
            assigned_ids.append(ent_id)

        upsert_res = self.client.upsert(self.collection, entities, **kwargs)

        result_ids: List[str] = []
        if hasattr(upsert_res, "results") and upsert_res.results:
            for idx, item in enumerate(upsert_res.results):
                item_id = getattr(item, "id", None)
                if item_id is not None:
                    res_id = str(item_id)
                elif idx < len(assigned_ids):
                    res_id = assigned_ids[idx]
                else:
                    res_id = str(getattr(item, "key", ""))
                result_ids.append(res_id)
            return result_ids
        return assigned_ids

    def similarity_search_with_score(
        self,
        query: str,
        k: int = 4,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        **kwargs: Any,
    ) -> List[Tuple[Document, float]]:
        """Return documents most similar to query text with distance score."""
        query_vector = self.embedding.embed_query(query)
        return self.similarity_search_by_vector_with_score(
            embedding=query_vector,
            k=k,
            filter=filter,
            **kwargs,
        )

    def similarity_search_by_vector_with_score(
        self,
        embedding: List[float],
        k: int = 4,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        **kwargs: Any,
    ) -> List[Tuple[Document, float]]:
        """Return documents most similar to embedding vector with distance score."""
        search_filter = _translate_filter(filter)
        include = kwargs.pop("include", ("attributes", "document"))
        search_res = self.client.search(
            collection=self.collection,
            vector=embedding,
            k=k,
            filter=search_filter,
            include=include,
            **kwargs,
        )

        docs_with_scores: List[Tuple[Document, float]] = []
        for hit in search_res.hits:
            metadata = dict(hit.plain_attributes())
            metadata["key"] = hit.key
            if hit.id is not None:
                metadata["id"] = hit.id
            doc = self._document_cls(
                page_content=hit.document or "",
                metadata=metadata,
            )
            docs_with_scores.append((doc, float(hit.distance)))

        return docs_with_scores

    def similarity_search(
        self,
        query: str,
        k: int = 4,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        **kwargs: Any,
    ) -> List[Document]:
        """Return documents most similar to query text."""
        docs_with_scores = self.similarity_search_with_score(
            query=query,
            k=k,
            filter=filter,
            **kwargs,
        )
        return [doc for doc, _ in docs_with_scores]

    def similarity_search_by_vector(
        self,
        embedding: List[float],
        k: int = 4,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        **kwargs: Any,
    ) -> List[Document]:
        """Return documents most similar to embedding vector."""
        docs_with_scores = self.similarity_search_by_vector_with_score(
            embedding=embedding,
            k=k,
            filter=filter,
            **kwargs,
        )
        return [doc for doc, _ in docs_with_scores]

    def delete(
        self,
        ids: Optional[Sequence[str]] = None,
        **kwargs: Any,
    ) -> bool:
        """Delete entities by IDs or other criteria."""
        if (ids is None or len(ids) == 0) and not kwargs:
            return True
        refs = list(ids) if ids is not None and len(ids) > 0 else None
        self.client.delete(self.collection, refs=refs, **kwargs)
        return True

    @classmethod
    def from_texts(
        cls: Type["ArkivectorVectorStore"],
        texts: List[str],
        embedding: Any,
        metadatas: Optional[List[Dict[str, Any]]] = None,
        *,
        client: Any,
        collection: str,
        ids: Optional[List[str]] = None,
        **kwargs: Any,
    ) -> "ArkivectorVectorStore":
        """Return VectorStore initialized from texts and embeddings."""
        store = cls(
            client=client,
            collection=collection,
            embedding=embedding,
            **kwargs,
        )
        store.add_texts(texts=texts, metadatas=metadatas, ids=ids)
        return store
