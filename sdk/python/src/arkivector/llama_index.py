"""LlamaIndex VectorStore adapter for Arkivector."""

from __future__ import annotations

import uuid
from typing import (
    TYPE_CHECKING,
    Any,
    Dict,
    List,
    Optional,
    Sequence,
    Tuple,
    Type,
)

from arkivector.filter import F
from arkivector.models import T

if TYPE_CHECKING:
    from llama_index.core.schema import BaseNode
    from llama_index.core.vector_stores.types import (
        BasePydanticVectorStore,
    )
else:
    try:
        from llama_index.core.schema import BaseNode
        from llama_index.core.vector_stores.types import BasePydanticVectorStore
    except ImportError:
        BasePydanticVectorStore = object
        BaseNode = object


def _get_llama_index_core() -> Tuple[Type[Any], Type[Any], Type[Any]]:
    """Lazy import of llama_index_core classes."""
    try:
        from llama_index.core.schema import TextNode
        from llama_index.core.vector_stores.types import (
            BasePydanticVectorStore as LCPydanticVectorStore,
        )
        from llama_index.core.vector_stores.types import (
            VectorStoreQueryResult as LCQueryResult,
        )

        return LCPydanticVectorStore, LCQueryResult, TextNode
    except ImportError as exc:
        raise ImportError(
            "llama-index-core is required to use ArkivectorVectorStore with LlamaIndex. "
            "Please install it with: pip install 'arkivector[llama-index]' or "
            "pip install llama-index-core"
        ) from exc


def _to_filter_value(val: Any) -> Any:
    if isinstance(val, float):
        return T.dec(val)
    return val


def _translate_single_filter(f: Any) -> Optional[F]:
    if not hasattr(f, "key") or not hasattr(f, "operator"):
        return None

    op = f.operator
    op_str = op.value if hasattr(op, "value") else str(op)
    op_str = op_str.lower()
    key = str(f.key)
    val = _to_filter_value(f.value)

    if op_str in ("==", "eq"):
        return F.eq(key, val)
    elif op_str in ("!=", "ne"):
        return F.ne(key, val)
    elif op_str in (">", "gt"):
        return F.gt(key, val)
    elif op_str in (">=", "gte"):
        return F.gte(key, val)
    elif op_str in ("<", "lt"):
        return F.lt(key, val)
    elif op_str in ("<=", "lte"):
        return F.lte(key, val)
    elif op_str == "in":
        values = list(val) if isinstance(val, (list, tuple, set)) else [val]
        if not values:
            return None
        items = [F.eq(key, _to_filter_value(v)) for v in values]
        return F.or_(*items) if len(items) > 1 else items[0]
    elif op_str == "nin":
        values = list(val) if isinstance(val, (list, tuple, set)) else [val]
        if not values:
            return None
        items = [F.eq(key, _to_filter_value(v)) for v in values]
        return F.not_(F.or_(*items) if len(items) > 1 else items[0])
    else:
        raise ValueError(f"Unsupported filter operator: {f.operator!r}")


def _translate_filters(filters: Any) -> Optional[F]:
    if filters is None:
        return None
    if isinstance(filters, F):
        return filters

    filter_list = getattr(filters, "filters", None)
    if filter_list is None and isinstance(filters, list):
        filter_list = filters
    if filter_list is None:
        if hasattr(filters, "key") and hasattr(filters, "operator"):
            filter_list = [filters]
        else:
            return None

    nodes: List[F] = []
    for f in filter_list:
        if hasattr(f, "filters"):
            nested = _translate_filters(f)
            if nested is not None:
                nodes.append(nested)
        elif hasattr(f, "key") and hasattr(f, "operator"):
            node = _translate_single_filter(f)
            if node is not None:
                nodes.append(node)
        elif isinstance(f, F):
            nodes.append(f)

    if not nodes:
        return None
    if len(nodes) == 1:
        return nodes[0]

    cond = getattr(filters, "condition", "and")
    if hasattr(cond, "value"):
        cond = cond.value
    if str(cond).lower() in ("or", "filtercondition.or"):
        return F.or_(*nodes)
    return F.and_(*nodes)


class ArkivectorVectorStore(BasePydanticVectorStore):  # type: ignore[misc]
    """Arkivector VectorStore implementation for LlamaIndex."""

    stores_text: bool = True

    def __init__(
        self,
        client: Any = None,
        collection: Optional[str] = None,
        collection_name: Optional[str] = None,
        **kwargs: Any,
    ) -> None:
        """Initialize Arkivector VectorStore for LlamaIndex.

        Raises:
            ImportError: If llama-index-core is not installed.
        """
        _, self._query_result_cls, self._node_cls = _get_llama_index_core()
        coll = collection or collection_name or "default"
        if hasattr(super(), "__init__") and super().__init__ is not object.__init__:
            try:
                super().__init__(collection=coll, **kwargs)
            except Exception:
                pass
        self._client = client
        self.collection = coll

    @property
    def client(self) -> Any:
        """Expose client instance."""
        return self._client

    def add(
        self,
        nodes: Sequence[Any],
        **kwargs: Any,
    ) -> List[str]:
        """Add nodes to index."""
        if not nodes:
            return []

        entities: List[Dict[str, Any]] = []
        node_ids: List[str] = []

        for node in nodes:
            node_id = (
                getattr(node, "node_id", None) or getattr(node, "id_", None) or uuid.uuid4().hex
            )
            text = (
                node.get_content()
                if hasattr(node, "get_content") and callable(node.get_content)
                else getattr(node, "text", "")
            )
            embedding = (
                node.get_embedding()
                if hasattr(node, "get_embedding") and callable(node.get_embedding)
                else getattr(node, "embedding", None)
            )
            metadata = getattr(node, "metadata", {}) or {}

            entities.append(
                {
                    "id": node_id,
                    "vector": embedding,
                    "document": text,
                    "attributes": dict(metadata),
                }
            )
            node_ids.append(node_id)

        upsert_res = self.client.upsert(self.collection, entities, **kwargs)
        result_ids: List[str] = []
        if hasattr(upsert_res, "results") and upsert_res.results:
            for idx, item in enumerate(upsert_res.results):
                item_id = getattr(item, "id", None)
                if item_id is not None:
                    res_id = str(item_id)
                elif idx < len(node_ids):
                    res_id = node_ids[idx]
                else:
                    res_id = str(getattr(item, "key", ""))
                result_ids.append(res_id)
            return result_ids
        return node_ids

    def delete(
        self,
        ref_doc_id: Optional[str] = None,
        ids: Optional[Sequence[str]] = None,
        **delete_kwargs: Any,
    ) -> None:
        """Delete nodes or ref doc from index."""
        target_ids: List[str] = []
        if ref_doc_id is not None:
            target_ids.append(ref_doc_id)
        if ids is not None:
            target_ids.extend(ids)
        if not target_ids and not delete_kwargs:
            return
        refs = target_ids if target_ids else None
        self.client.delete(self.collection, refs=refs, **delete_kwargs)

    def query(
        self,
        query: Any,
        **kwargs: Any,
    ) -> Any:
        """Query index for top k most similar nodes."""
        query_embedding = getattr(query, "query_embedding", None)
        if query_embedding is None:
            raise ValueError("query_embedding is required in VectorStoreQuery")

        top_k = getattr(query, "similarity_top_k", 4) or 4
        search_filter = _translate_filters(getattr(query, "filters", None))

        node_ids = getattr(query, "node_ids", None)
        if node_ids:
            nid_filter = (
                F.or_(*[F.eq("id", nid) for nid in node_ids])
                if len(node_ids) > 1
                else F.eq("id", node_ids[0])
            )
            if search_filter is not None:
                search_filter = F.and_(search_filter, nid_filter)
            else:
                search_filter = nid_filter

        include = kwargs.pop("include", ("attributes", "document"))
        search_res = self.client.search(
            collection=self.collection,
            vector=query_embedding,
            k=top_k,
            filter=search_filter,
            include=include,
            **kwargs,
        )

        nodes: List[Any] = []
        similarities: List[float] = []
        ids: List[str] = []

        for hit in search_res.hits:
            hit_id = hit.id or hit.key
            meta = dict(hit.plain_attributes())
            meta["key"] = hit.key
            if hit.id is not None:
                meta["id"] = hit.id

            node = self._node_cls(
                text=hit.document or "",
                id_=hit_id,
                metadata=meta,
            )
            nodes.append(node)
            score = float(hit.score) if hit.score is not None else float(1.0 - hit.distance)
            similarities.append(score)
            ids.append(hit_id)

        return self._query_result_cls(
            nodes=nodes,
            similarities=similarities,
            ids=ids,
        )
