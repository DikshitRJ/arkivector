"""Arkivector API client."""

import base64
import os
import re
import threading
import urllib.parse
import uuid
from decimal import Decimal
from typing import Any, Dict, Generator, List, Optional, Sequence, Tuple, Union

from arkivector._http import HttpTransport
from arkivector.errors import ApiError, NotImplementedByDaemon
from arkivector.filter import F
from arkivector.models import (
    BatchResult,
    Collection,
    ColumnDef,
    Consistency,
    CountResult,
    DirectAttribute,
    Entity,
    EntityInput,
    IndexDef,
    PrepareResult,
    Readiness,
    ScrollPage,
    SearchResult,
    SyncStatus,
    Ttl,
    TxResult,
    TypedValue,
    UpsertResult,
    VersionInfo,
)
from arkivector.types import Bit, HalfVector, SparseVector, Vector
from arkivector.vectorbytes import encode_halfvec, encode_vector

SUPPORTED_API_MAJOR = 1


class Client:
    """Client for the Arkivector vector co-processor daemon."""

    def __init__(
        self,
        url: Optional[str] = None,
        api_key: Optional[str] = None,
        timeout: float = 10.0,
        max_retries: int = 3,
        session_consistency: bool = False,
        idempotency: bool = True,
    ) -> None:
        raw_url = url or os.environ.get("ARKIVECTOR_URL") or "http://127.0.0.1:8080"
        self.url = raw_url.rstrip("/")
        self.api_key = api_key or os.environ.get("ARKIVECTOR_API_KEY")
        self.timeout = float(timeout)
        self.max_retries = int(max_retries)
        self.session_consistency = bool(session_consistency)
        self.idempotency = bool(idempotency)
        self.last_write_block: Optional[int] = None

        self._version_info: Optional[VersionInfo] = None
        self._collection_column_types: Dict[str, str] = {}
        self._lock = threading.Lock()

        self._http = HttpTransport(
            base_url=self.url,
            api_key=self.api_key,
            timeout=self.timeout,
            max_retries=self.max_retries,
        )

    @property
    def last_written_block(self) -> Optional[int]:
        """Alias for last_write_block."""
        return self.last_write_block

    @last_written_block.setter
    def last_written_block(self, val: Optional[int]) -> None:
        self.last_write_block = val

    def version(self) -> VersionInfo:
        """Fetch daemon version, API version, and capabilities (lazily cached)."""
        if self._version_info is not None:
            return self._version_info
        with self._lock:
            if self._version_info is not None:
                return self._version_info
            resp = self._http.request("GET", "/v1/version", is_idempotent=True, auth=False)
            data = resp.json() or {}
            vinfo = VersionInfo.from_json(data)
            major = self._extract_api_major(vinfo.api_version)
            if major != SUPPORTED_API_MAJOR:
                raise ApiError(
                    code="unsupported_api_version",
                    status=0,
                    title="Unsupported API version",
                    detail=(
                        f"Daemon API version {vinfo.api_version!r} is incompatible "
                        f"with SDK major {SUPPORTED_API_MAJOR}"
                    ),
                )
            self._version_info = vinfo
            return vinfo

    @staticmethod
    def _extract_api_major(api_version: str) -> int:
        try:
            return int(api_version.split(".")[0])
        except (ValueError, IndexError):
            return -1

    def has_capability(self, name: str) -> bool:
        """Check whether the daemon advertises the given capability."""
        vinfo = self.version()
        return name in vinfo.capabilities

    def check_capability(self, name: str) -> None:
        """Raise NotImplementedByDaemon locally if the daemon lacks the requested capability."""
        if not self.has_capability(name):
            raise NotImplementedByDaemon(
                code="not_implemented",
                status=501,
                title="Not implemented",
                detail=f"Daemon does not support capability {name!r}",
            )

    def health(self) -> bool:
        """Liveness probe (GET /healthz). Returns True if process is healthy."""
        try:
            resp = self._http.request("GET", "/healthz", is_idempotent=True, auth=False)
            data = resp.json()
            return bool(data and data.get("status") == "ok")
        except ApiError:
            return False

    def ready(self) -> Readiness:
        """Readiness probe (GET /readyz)."""
        resp = self._http.request("GET", "/readyz", is_idempotent=True, auth=False)
        data = resp.json() or {}
        return Readiness.from_json(data)

    def sync_status(self) -> List[SyncStatus]:
        """Fetch sync status for all tracked collections (GET /v1/sync)."""
        self.check_capability("ops.sync")
        resp = self._http.request("GET", "/v1/sync", is_idempotent=True)
        data = resp.json() or {}
        items = data.get("collections", [])
        return [SyncStatus.from_json(item) for item in items]

    def list_collections(self) -> List[Collection]:
        """List all collections defined on this daemon (GET /v1/collections)."""
        self.check_capability("collections.read")
        resp = self._http.request("GET", "/v1/collections", is_idempotent=True)
        data = resp.json() or {}
        raw_list = data.get("collections", [])
        cols = [Collection.from_json(item) for item in raw_list]
        with self._lock:
            for c in cols:
                self._collection_column_types[c.name] = c.column.type
        return cols

    def get_collection(self, name: str) -> Collection:
        """Describe a single collection by name (GET /v1/collections/{name})."""
        self.check_capability("collections.read")
        encoded_name = urllib.parse.quote(name, safe="")
        resp = self._http.request("GET", f"/v1/collections/{encoded_name}", is_idempotent=True)
        data = resp.json() or {}
        col = Collection.from_json(data)
        with self._lock:
            self._collection_column_types[col.name] = col.column.type
        return col

    def create_collection(
        self,
        name: str,
        column: Union[ColumnDef, Tuple[str, int], Dict[str, Any]],
        index: Union[IndexDef, Dict[str, Any]],
        *,
        schema_tag: Optional[str] = None,
        writers: Optional[List[str]] = None,
        membership: str = "creator",
        attributes: Optional[Dict[str, str]] = None,
        default_ttl: Optional[Union[Ttl, Dict[str, Any]]] = None,
        publish_manifest: bool = False,
        model: Optional[str] = None,
        mode: str = "full",
    ) -> Collection:
        """Create a new collection and start syncing it (POST /v1/collections)."""
        self.check_capability("collections.write")
        if publish_manifest:
            self.check_capability("collections.manifest")

        if isinstance(column, tuple):
            col_dict: Dict[str, Any] = {"type": column[0], "dimensions": column[1]}
        elif isinstance(column, ColumnDef):
            col_dict = column.to_json()
        elif isinstance(column, dict):
            col_dict = column
        else:
            raise ValueError(f"Invalid column definition: {column!r}")

        if isinstance(index, IndexDef):
            idx_dict = index.to_json()
        elif isinstance(index, dict):
            idx_dict = dict(index)
            if "with_params" in idx_dict and "with" not in idx_dict:
                idx_dict["with"] = idx_dict.pop("with_params")
        else:
            raise ValueError(f"Invalid index definition: {index!r}")

        payload: Dict[str, Any] = {
            "name": name,
            "column": col_dict,
            "index": idx_dict,
        }
        if schema_tag is not None:
            payload["schema_tag"] = schema_tag
        if writers is not None:
            payload["writers"] = writers
        if membership is not None:
            payload["membership"] = membership
        if attributes is not None:
            payload["attributes"] = attributes
        if default_ttl is not None:
            payload["default_ttl"] = (
                default_ttl.to_json() if isinstance(default_ttl, Ttl) else default_ttl
            )
        if publish_manifest:
            payload["publish_manifest"] = True
        if model is not None:
            payload["model"] = model
        if mode is not None:
            payload["mode"] = mode

        resp = self._http.request("POST", "/v1/collections", body=payload)
        data = resp.json() or {}
        col = Collection.from_json(data)
        with self._lock:
            self._collection_column_types[col.name] = col.column.type
        return col

    def follow_collection(
        self,
        manifest_key: Optional[str] = None,
        *,
        name: Optional[str] = None,
        definition: Optional[Union[Dict[str, Any], Collection]] = None,
    ) -> Collection:
        """Follow an existing collection by manifest key or definition.

        Endpoint: POST /v1/collections:follow
        """
        self.check_capability("collections.follow")
        payload: Dict[str, Any] = {}
        if manifest_key is not None:
            payload["manifest_key"] = manifest_key
            if name is not None:
                payload["name"] = name
        elif definition is not None:
            if isinstance(definition, Collection):
                payload["definition"] = {
                    "name": definition.name,
                    "schema_tag": definition.schema_tag,
                    "column": definition.column.to_json(),
                    "index": definition.index.to_json(),
                    "writers": definition.writers,
                    "membership": definition.membership,
                    "attributes": definition.attributes,
                    "default_ttl": (
                        definition.default_ttl.to_json() if definition.default_ttl else None
                    ),
                    "mode": definition.mode,
                }
                if definition.model is not None:
                    payload["definition"]["model"] = definition.model
            elif isinstance(definition, dict):
                payload["definition"] = definition
            else:
                raise ValueError(f"Invalid definition: {definition!r}")
        else:
            raise ValueError("Must provide either manifest_key or definition to follow_collection")

        resp = self._http.request("POST", "/v1/collections:follow", body=payload)
        data = resp.json() or {}
        col = Collection.from_json(data)
        with self._lock:
            self._collection_column_types[col.name] = col.column.type
        return col

    def delete_collection(self, name: str) -> None:
        """Stop tracking a collection and drop local index data (DELETE /v1/collections/{name})."""
        self.check_capability("collections.write")
        encoded_name = urllib.parse.quote(name, safe="")
        self._http.request("DELETE", f"/v1/collections/{encoded_name}")
        with self._lock:
            self._collection_column_types.pop(name, None)

    def get_cached_column_type(self, collection_name: str) -> Optional[str]:
        """Return the cached column type ('vector', 'halfvec', etc.) for a collection."""
        with self._lock:
            return self._collection_column_types.get(collection_name)

    def _serialize_search_vector(
        self, vector: Any, col_type: Optional[str]
    ) -> Tuple[Optional[Any], Optional[str]]:
        """Return (vector_json, vector_bytes_b64). Exactly one is not None."""
        if isinstance(vector, Vector) and col_type == "vector":
            b64 = base64.b64encode(vector.to_vectorbytes()).decode("ascii")
            return None, b64
        if isinstance(vector, HalfVector) and col_type == "halfvec":
            b64 = base64.b64encode(vector.to_vectorbytes()).decode("ascii")
            return None, b64
        if isinstance(vector, Bit) and col_type == "bit":
            b64 = base64.b64encode(vector.to_vectorbytes()).decode("ascii")
            return None, b64
        if isinstance(vector, SparseVector) and col_type == "sparsevec":
            b64 = base64.b64encode(vector.to_vectorbytes()).decode("ascii")
            return None, b64

        if isinstance(vector, Bit):
            return vector.to_text(), None
        if isinstance(vector, SparseVector):
            return vector.to_text(), None
        if (
            hasattr(vector, "to_list")
            and callable(vector.to_list)
            and not isinstance(vector, (str, bytes))
        ):
            return [float(x) for x in vector.to_list()], None
        if isinstance(vector, (list, tuple)):
            return list(vector), None
        if isinstance(vector, dict):
            return vector, None
        if isinstance(vector, str):
            return vector, None

        raise TypeError(f"Unsupported vector type: {type(vector).__name__}")

    def get(
        self,
        collection: str,
        ref: str,
        include: Sequence[str] = ("attributes",),
        verify: bool = False,
        min_block: Optional[int] = None,
        timeout_ms: Optional[int] = None,
    ) -> Entity:
        """Point lookup by key or external id (GET /v1/collections/{name}/entities/{ref})."""
        self.check_capability("entities.read")
        if verify:
            self.check_capability("entities.verify")

        is_hex_key = bool(re.match(r"^0x[0-9a-fA-F]{64}$", ref))
        encoded_ref = ref if is_hex_key else urllib.parse.quote(ref, safe="")
        encoded_col = urllib.parse.quote(collection, safe="")

        params: Dict[str, Union[str, int]] = {}
        if include is not None:
            params["include"] = (
                ",".join(include) if isinstance(include, (list, tuple, set)) else str(include)
            )
        if verify:
            params["verify"] = "true"
        if min_block is None and self.session_consistency and self.last_write_block is not None:
            min_block = self.last_write_block
        if min_block is not None:
            params["min_block"] = int(min_block)
        if timeout_ms is not None:
            params["timeout_ms"] = int(timeout_ms)

        resp = self._http.request(
            "GET",
            f"/v1/collections/{encoded_col}/entities/{encoded_ref}",
            query=params,
            is_idempotent=True,
        )
        data = resp.json() or {}
        entity_dict = data.get("entity", data)
        return Entity.from_json(entity_dict)

    def search(
        self,
        collection: str,
        vector: Any,
        k: int = 10,
        offset: int = 0,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        where: Optional[str] = None,
        operator: Optional[str] = None,
        ef_search: Optional[int] = None,
        iterative_scan: Optional[str] = None,
        max_scan_tuples: Optional[int] = None,
        max_distance: Optional[float] = None,
        min_score: Optional[float] = None,
        exact: bool = False,
        include: Sequence[str] = ("attributes",),
        consistency: Optional[Union[Consistency, Dict[str, Any]]] = None,
        explain: bool = False,
        min_block: Optional[int] = None,
        timeout_ms: Optional[int] = None,
    ) -> SearchResult:
        """Vector similarity search (POST /v1/collections/{name}/search)."""
        col_type = self.get_cached_column_type(collection)
        if col_type is None and isinstance(vector, (Vector, HalfVector, Bit, SparseVector)):
            try:
                col = self.get_collection(collection)
                col_type = col.column.type
            except Exception:
                col_type = None

        v_json, v_b64 = self._serialize_search_vector(vector, col_type)

        if exact:
            self.check_capability("search.exact")
        if filter is not None:
            self.check_capability("search.filter.json")
        if where is not None:
            self.check_capability("search.filter.where")
        if explain:
            self.check_capability("search.explain")

        body: Dict[str, Any] = {"k": int(k)}
        if offset != 0:
            body["offset"] = int(offset)
        if v_b64 is not None:
            body["vector_bytes"] = v_b64
        else:
            body["vector"] = v_json

        if filter is not None and where is not None:
            raise ValueError("Cannot specify both filter and where")
        if filter is not None:
            body["filter"] = filter.to_dict() if hasattr(filter, "to_dict") else filter
        if where is not None:
            body["where"] = str(where)

        if operator is not None:
            body["operator"] = str(operator)
        if ef_search is not None:
            body["ef_search"] = int(ef_search)
        if iterative_scan is not None:
            body["iterative_scan"] = str(iterative_scan)
        if max_scan_tuples is not None:
            body["max_scan_tuples"] = int(max_scan_tuples)
        if max_distance is not None:
            body["max_distance"] = float(max_distance)
        if min_score is not None:
            body["min_score"] = float(min_score)
        if exact:
            body["exact"] = True
        if explain:
            body["explain"] = True
        if include is not None:
            body["include"] = (
                list(include) if isinstance(include, (list, tuple, set)) else [include]
            )

        if consistency is None:
            if min_block is not None or timeout_ms is not None:
                consistency = Consistency(min_block=min_block, timeout_ms=timeout_ms)
            elif self.session_consistency and self.last_write_block is not None:
                consistency = Consistency(min_block=self.last_write_block)

        if consistency is not None:
            body["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST", f"/v1/collections/{encoded_name}/search", body=body, is_idempotent=True
        )
        data = resp.json() or {}
        return SearchResult.from_json(data)

    def search_batch(
        self,
        collection: str,
        searches: Sequence[Union[Dict[str, Any], Any]],
        consistency: Optional[Union[Consistency, Dict[str, Any]]] = None,
    ) -> BatchResult:
        """Execute multiple searches against a single snapshot.

        Endpoint: POST /v1/collections/{name}/search:batch
        """
        col_type = self.get_cached_column_type(collection)
        if col_type is None:
            try:
                col = self.get_collection(collection)
                col_type = col.column.type
            except Exception:
                col_type = None

        prepared: List[Dict[str, Any]] = []
        for s in searches:
            s_req = dict(s) if isinstance(s, dict) else dict(s.__dict__)
            if "vector" in s_req and "vector_bytes" not in s_req:
                vec = s_req["vector"]
                v_json, v_b64 = self._serialize_search_vector(vec, col_type)
                if v_b64 is not None:
                    s_req.pop("vector", None)
                    s_req["vector_bytes"] = v_b64
                elif v_json is not None:
                    s_req["vector"] = v_json

            if "filter" in s_req and hasattr(s_req["filter"], "to_dict"):
                s_req["filter"] = s_req["filter"].to_dict()

            if s_req.get("exact"):
                self.check_capability("search.exact")
            if "filter" in s_req and s_req["filter"] is not None:
                self.check_capability("search.filter.json")
            if "where" in s_req and s_req["where"] is not None:
                self.check_capability("search.filter.where")
            if s_req.get("explain"):
                self.check_capability("search.explain")

            prepared.append(s_req)

        payload: Dict[str, Any] = {"searches": prepared}
        if consistency is None and self.session_consistency and self.last_write_block is not None:
            consistency = Consistency(min_block=self.last_write_block)

        if consistency is not None:
            payload["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST",
            f"/v1/collections/{encoded_name}/search:batch",
            body=payload,
            is_idempotent=True,
        )
        data = resp.json() or {}
        return BatchResult.from_json(data)

    def scroll_page(
        self,
        collection: str,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        where: Optional[str] = None,
        limit: int = 100,
        cursor: Optional[str] = None,
        include: Sequence[str] = ("attributes",),
        consistency: Optional[Union[Consistency, Dict[str, Any]]] = None,
        min_block: Optional[int] = None,
        timeout_ms: Optional[int] = None,
    ) -> ScrollPage:
        """Fetch a single page of entities (POST /v1/collections/{name}/scroll)."""
        self.check_capability("scroll")
        if filter is not None:
            self.check_capability("search.filter.json")
        if where is not None:
            self.check_capability("search.filter.where")
        if filter is not None and where is not None:
            raise ValueError("Cannot specify both filter and where")

        body: Dict[str, Any] = {"limit": int(limit)}
        if cursor is not None:
            body["cursor"] = str(cursor)
        if filter is not None:
            body["filter"] = filter.to_dict() if hasattr(filter, "to_dict") else filter
        if where is not None:
            body["where"] = str(where)
        if include is not None:
            body["include"] = (
                list(include) if isinstance(include, (list, tuple, set)) else [include]
            )

        if consistency is None:
            if min_block is not None or timeout_ms is not None:
                consistency = Consistency(min_block=min_block, timeout_ms=timeout_ms)
            elif self.session_consistency and self.last_write_block is not None:
                consistency = Consistency(min_block=self.last_write_block)

        if consistency is not None:
            body["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST", f"/v1/collections/{encoded_name}/scroll", body=body, is_idempotent=True
        )
        data = resp.json() or {}
        return ScrollPage.from_json(data)

    def scroll(
        self,
        collection: str,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        where: Optional[str] = None,
        limit: int = 100,
        include: Sequence[str] = ("attributes",),
        consistency: Optional[Union[Consistency, Dict[str, Any]]] = None,
        min_block: Optional[int] = None,
        timeout_ms: Optional[int] = None,
    ) -> Generator[Entity, None, None]:
        """Enumerate live entities matching a filter in ascending key order across pages."""
        cursor: Optional[str] = None
        while True:
            page = self.scroll_page(
                collection,
                filter=filter,
                where=where,
                limit=limit,
                cursor=cursor,
                include=include,
                consistency=consistency,
                min_block=min_block,
                timeout_ms=timeout_ms,
            )
            for entity in page.entities:
                yield entity
            if not page.next_cursor:
                break
            cursor = page.next_cursor

    def count(
        self,
        collection: str,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        where: Optional[str] = None,
        consistency: Optional[Union[Consistency, Dict[str, Any]]] = None,
        min_block: Optional[int] = None,
        timeout_ms: Optional[int] = None,
    ) -> CountResult:
        """Exact count of live matching entities (POST /v1/collections/{name}/count)."""
        self.check_capability("count")
        if filter is not None:
            self.check_capability("search.filter.json")
        if where is not None:
            self.check_capability("search.filter.where")
        if filter is not None and where is not None:
            raise ValueError("Cannot specify both filter and where")

        body: Dict[str, Any] = {}
        if filter is not None:
            body["filter"] = filter.to_dict() if hasattr(filter, "to_dict") else filter
        if where is not None:
            body["where"] = str(where)

        if consistency is None:
            if min_block is not None or timeout_ms is not None:
                consistency = Consistency(min_block=min_block, timeout_ms=timeout_ms)
            elif self.session_consistency and self.last_write_block is not None:
                consistency = Consistency(min_block=self.last_write_block)

        if consistency is not None:
            body["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST", f"/v1/collections/{encoded_name}/count", body=body, is_idempotent=True
        )
        data = resp.json() or {}
        return CountResult.from_json(data)

    def upsert(
        self,
        collection: str,
        entities: Sequence[Union[EntityInput, Dict[str, Any]]],
        wait: str = "receipt",
        timeout_ms: int = 60000,
        idempotency_key: Optional[str] = None,
    ) -> UpsertResult:
        """Upsert entities through the daemon's managed signer.

        Endpoint: POST /v1/collections/{name}/entities
        """
        self.check_capability("entities.write")
        if not entities:
            raise ValueError("entities list cannot be empty")
        if len(entities) > 1000:
            raise ValueError("entities list exceeds maximum of 1,000 items")

        prepared_entities: List[Dict[str, Any]] = []
        for e in entities:
            if isinstance(e, EntityInput):
                prepared_entities.append(e.to_json())
            elif isinstance(e, dict):
                prepared_entities.append(EntityInput.from_json(e).to_json())
            else:
                raise TypeError(f"Invalid entity type: {type(e).__name__}")

        body: Dict[str, Any] = {
            "entities": prepared_entities,
            "wait": str(wait),
            "timeout_ms": int(timeout_ms),
        }

        if idempotency_key is None and self.idempotency:
            idempotency_key = str(uuid.uuid4())

        http_timeout = max(self.timeout, (float(timeout_ms) / 1000.0) + 5.0)
        encoded_col = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST",
            f"/v1/collections/{encoded_col}/entities",
            body=body,
            idempotency_key=idempotency_key,
            timeout=http_timeout,
        )
        data = resp.json() or {}
        res = UpsertResult.from_json(data)
        if res.block_number is not None:
            self.last_write_block = max(self.last_write_block or 0, res.block_number)
        return res

    def prepare(
        self,
        collection: str,
        entities: Sequence[Union[EntityInput, Dict[str, Any]]],
        from_address: str,
    ) -> PrepareResult:
        """Build unsigned execute(ops) transactions for client-side signing.

        Endpoint: POST /v1/collections/{name}/entities:prepare
        """
        self.check_capability("entities.prepare")
        if not entities:
            raise ValueError("entities list cannot be empty")
        if len(entities) > 1000:
            raise ValueError("entities list exceeds maximum of 1,000 items")

        prepared_entities: List[Dict[str, Any]] = []
        for e in entities:
            if isinstance(e, EntityInput):
                prepared_entities.append(e.to_json())
            elif isinstance(e, dict):
                prepared_entities.append(EntityInput.from_json(e).to_json())
            else:
                raise TypeError(f"Invalid entity type: {type(e).__name__}")

        body: Dict[str, Any] = {
            "entities": prepared_entities,
            "from": str(from_address),
        }

        encoded_col = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST",
            f"/v1/collections/{encoded_col}/entities:prepare",
            body=body,
            is_idempotent=True,
        )
        data = resp.json() or {}
        return PrepareResult.from_json(data)

    def delete(
        self,
        collection: str,
        refs: Optional[Sequence[str]] = None,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        where: Optional[str] = None,
        idempotency_key: Optional[str] = None,
    ) -> TxResult:
        """Delete entities by refs, filter, or where clause.

        Endpoint: POST /v1/collections/{name}/entities:delete
        (or DELETE /v1/collections/{name}/entities/{ref} if single ref and no filter/where).
        """
        self.check_capability("entities.write")
        if filter is not None:
            self.check_capability("search.filter.json")
        if where is not None:
            self.check_capability("search.filter.where")

        has_refs = refs is not None and len(refs) > 0
        has_filter = filter is not None
        has_where = where is not None

        count_targets = sum([has_refs, has_filter, has_where])
        if count_targets != 1:
            raise ValueError("Must specify exactly one of refs, filter, or where")

        if idempotency_key is None and self.idempotency:
            idempotency_key = str(uuid.uuid4())

        encoded_col = urllib.parse.quote(collection, safe="")

        if refs is not None and len(refs) == 1 and not has_filter and not has_where:
            ref = refs[0]
            is_hex_key = bool(re.match(r"^0x[0-9a-fA-F]{64}$", ref))
            encoded_ref = ref if is_hex_key else urllib.parse.quote(ref, safe="")
            resp = self._http.request(
                "DELETE",
                f"/v1/collections/{encoded_col}/entities/{encoded_ref}",
                idempotency_key=idempotency_key,
            )
            data = resp.json() or {}
            res = TxResult.from_json(data)
            if res.block_number is not None:
                self.last_write_block = max(self.last_write_block or 0, res.block_number)
            return res

        body: Dict[str, Any] = {}
        if refs is not None:
            if len(refs) > 1000:
                raise ValueError("refs list exceeds maximum of 1,000 items")
            body["refs"] = list(refs)
        elif filter is not None:
            body["filter"] = filter.to_dict() if hasattr(filter, "to_dict") else filter
        elif where is not None:
            body["where"] = str(where)

        resp = self._http.request(
            "POST",
            f"/v1/collections/{encoded_col}/entities:delete",
            body=body,
            idempotency_key=idempotency_key,
        )
        data = resp.json() or {}
        res = TxResult.from_json(data)
        if res.block_number is not None:
            self.last_write_block = max(self.last_write_block or 0, res.block_number)
        return res

    def extend(
        self,
        collection: str,
        ttl: Union[Ttl, Dict[str, Any]],
        refs: Optional[Sequence[str]] = None,
        filter: Optional[Union[F, Dict[str, Any]]] = None,
        where: Optional[str] = None,
        idempotency_key: Optional[str] = None,
    ) -> TxResult:
        """Extend entity TTL by refs, filter, or where clause.

        Endpoint: POST /v1/collections/{name}/entities:extend
        """
        self.check_capability("entities.write")
        if filter is not None:
            self.check_capability("search.filter.json")
        if where is not None:
            self.check_capability("search.filter.where")

        has_refs = refs is not None and len(refs) > 0
        has_filter = filter is not None
        has_where = where is not None

        count_targets = sum([has_refs, has_filter, has_where])
        if count_targets != 1:
            raise ValueError("Must specify exactly one of refs, filter, or where")

        body: Dict[str, Any] = {
            "ttl": ttl.to_json() if isinstance(ttl, Ttl) else ttl,
        }
        if refs is not None:
            if len(refs) > 1000:
                raise ValueError("refs list exceeds maximum of 1,000 items")
            body["refs"] = list(refs)
        elif filter is not None:
            body["filter"] = filter.to_dict() if hasattr(filter, "to_dict") else filter
        elif where is not None:
            body["where"] = str(where)

        if idempotency_key is None and self.idempotency:
            idempotency_key = str(uuid.uuid4())

        encoded_col = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST",
            f"/v1/collections/{encoded_col}/entities:extend",
            body=body,
            idempotency_key=idempotency_key,
        )
        data = resp.json() or {}
        res = TxResult.from_json(data)
        if res.block_number is not None:
            self.last_write_block = max(self.last_write_block or 0, res.block_number)
        return res

    @staticmethod
    def build_entity_attributes(
        collection_def: Union[Collection, Dict[str, Any]],
        entity: Union[EntityInput, Dict[str, Any]],
    ) -> List[Tuple[str, int, bytes]]:
        """Direct-write attribute builder for on-chain entity creation."""
        return build_entity_attributes(collection_def, entity)

    def close(self) -> None:
        """Close client connections."""
        self._http.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.close()


def _serialize_attribute_value(val: Any, declared_type: Optional[str] = None) -> Tuple[int, bytes]:
    """Serialize a user attribute value to (type_id, value_bytes)."""
    if isinstance(val, DirectAttribute):
        return val.type_id, val.value_bytes
    if (
        isinstance(val, tuple)
        and len(val) == 2
        and isinstance(val[0], int)
        and isinstance(val[1], (bytes, bytearray))
    ):
        return val[0], bytes(val[1])

    if isinstance(val, TypedValue):
        t_name = val.type
        t_val = val.value
    elif isinstance(val, dict) and "type" in val and "value" in val:
        t_name = str(val["type"])
        t_val = val["value"]
    elif declared_type is not None:
        t_name = declared_type
        t_val = val
    elif isinstance(val, bool):
        t_name = "bool"
        t_val = val
    elif isinstance(val, int):
        t_name = "i32"
        t_val = val
    elif isinstance(val, float):
        t_name = "dec"
        t_val = str(val)
    elif isinstance(val, (bytes, bytearray)):
        t_name = "bytes"
        t_val = bytes(val)
    elif isinstance(val, str):
        t_name = "str"
        t_val = val
    else:
        t_name = "str"
        t_val = str(val)

    if t_name == "bool":
        b_val = bool(t_val) if not isinstance(t_val, str) else (t_val.lower() in ("true", "1"))
        return 0x01, (1 if b_val else 0).to_bytes(32, "big")
    elif t_name == "i32":
        return 0x02, int(t_val).to_bytes(32, "big", signed=True)
    elif t_name == "u64":
        return 0x03, int(t_val).to_bytes(32, "big", signed=False)
    elif t_name == "u256":
        return 0x04, int(t_val).to_bytes(32, "big", signed=False)
    elif t_name == "dec":
        d = Decimal(str(t_val))
        scaled = int(d * Decimal(10**18))
        return 0x05, scaled.to_bytes(32, "big", signed=True)
    elif t_name == "bytes":
        if isinstance(t_val, (bytes, bytearray)):
            raw_b = bytes(t_val)
        elif isinstance(t_val, str):
            raw_b = base64.b64decode(t_val)
        else:
            raw_b = bytes(t_val)
        return 0x07, raw_b
    elif t_name == "str":
        return 0x08, str(t_val).encode("utf-8")
    elif t_name == "addr":
        clean_addr = str(t_val).lower().removeprefix("0x")
        addr_bytes = bytes.fromhex(clean_addr)
        return 0x09, addr_bytes.rjust(32, b"\x00")
    elif t_name == "key":
        clean_key = str(t_val).lower().removeprefix("0x")
        return 0x0A, bytes.fromhex(clean_key)
    else:
        return 0x08, str(t_val).encode("utf-8")


def build_entity_attributes(
    collection_def: Union[Collection, Dict[str, Any]],
    entity: Union[EntityInput, Dict[str, Any]],
) -> List[Tuple[str, int, bytes]]:
    """Build on-chain attribute list according to Arkiv entity layout.

    Produces:
    1. arkiv.schema (0x88, schema_tag)
    2. av.vec (0x07, exact VectorBytes v1 matching collection column)
    3. av.id (0x88, external id UTF-8 bytes if present)
    4. av.doc (0x07, UTF-8 bytes of document if present)
    5. User attributes sorted by name.
    """
    if isinstance(collection_def, Collection):
        schema_tag = collection_def.schema_tag
        col_type = collection_def.column.type
        col_dim = collection_def.column.dimensions
        col_attrs = collection_def.attributes
    elif isinstance(collection_def, dict):
        schema_tag = str(
            collection_def.get("schema_tag") or f"arkivector.v1.{collection_def.get('name', '')}"
        )
        col_info = collection_def.get("column", {})
        if isinstance(col_info, ColumnDef):
            col_type = col_info.type
            col_dim = col_info.dimensions
        elif isinstance(col_info, dict):
            col_type = str(col_info.get("type", ""))
            col_dim = int(col_info.get("dimensions", 0))
        elif isinstance(col_info, (list, tuple)) and len(col_info) >= 2:
            col_type = str(col_info[0])
            col_dim = int(col_info[1])
        else:
            col_type = ""
            col_dim = 0
        raw_attrs = collection_def.get("attributes", {})
        col_attrs = dict(raw_attrs) if isinstance(raw_attrs, dict) else {}
    else:
        raise ValueError(f"Invalid collection definition: {type(collection_def).__name__}")

    if isinstance(entity, EntityInput):
        e_id = entity.id
        e_vec = entity.vector
        e_vec_bytes = entity.vector_bytes
        e_doc = entity.document
        e_attrs = entity.attributes or {}
    elif isinstance(entity, dict):
        e_id = entity.get("id")
        e_vec = entity.get("vector")
        e_vec_bytes = entity.get("vector_bytes")
        e_doc = entity.get("document")
        e_attrs = entity.get("attributes") or {}
    else:
        raise ValueError(f"Invalid entity: {type(entity).__name__}")

    raw_vb: bytes
    if e_vec_bytes is not None:
        if isinstance(e_vec_bytes, (bytes, bytearray)):
            raw_vb = bytes(e_vec_bytes)
        else:
            raw_vb = base64.b64decode(str(e_vec_bytes))
    elif e_vec is not None:
        if col_type == "vector":
            if isinstance(e_vec, Vector):
                raw_vb = e_vec.to_vectorbytes()
            elif (
                hasattr(e_vec, "to_list")
                and callable(e_vec.to_list)
                and not isinstance(e_vec, (str, bytes))
            ):
                raw_vb = encode_vector([float(x) for x in e_vec.to_list()])
            elif isinstance(e_vec, (list, tuple)):
                raw_vb = encode_vector([float(x) for x in e_vec])
            elif isinstance(e_vec, str):
                raw_vb = Vector.from_text(e_vec).to_vectorbytes()
            else:
                raise TypeError(
                    f"Cannot encode {type(e_vec).__name__} to VectorBytes for vector column"
                )
        elif col_type == "halfvec":
            if isinstance(e_vec, HalfVector):
                raw_vb = e_vec.to_vectorbytes()
            elif (
                hasattr(e_vec, "to_list")
                and callable(e_vec.to_list)
                and not isinstance(e_vec, (str, bytes))
            ):
                raw_vb = encode_halfvec([float(x) for x in e_vec.to_list()])
            elif isinstance(e_vec, (list, tuple)):
                raw_vb = encode_halfvec([float(x) for x in e_vec])
            elif isinstance(e_vec, str):
                raw_vb = HalfVector.from_text(e_vec).to_vectorbytes()
            else:
                raise TypeError(
                    f"Cannot encode {type(e_vec).__name__} to VectorBytes for halfvec column"
                )
        elif col_type == "bit":
            if isinstance(e_vec, Bit):
                raw_vb = e_vec.to_vectorbytes()
            elif isinstance(e_vec, str):
                raw_vb = Bit.from_text(e_vec).to_vectorbytes()
            elif isinstance(e_vec, (bytes, bytearray)):
                raw_vb = Bit(bytes(e_vec), bit_len=col_dim).to_vectorbytes()
            elif isinstance(e_vec, (list, tuple)):
                raw_vb = Bit(e_vec).to_vectorbytes()
            else:
                raise TypeError(
                    f"Cannot encode {type(e_vec).__name__} to VectorBytes for bit column"
                )
        elif col_type == "sparsevec":
            if isinstance(e_vec, SparseVector):
                raw_vb = e_vec.to_vectorbytes()
            elif isinstance(e_vec, str):
                raw_vb = SparseVector.from_text(e_vec).to_vectorbytes()
            elif isinstance(e_vec, dict):
                if "indices" in e_vec and "values" in e_vec:
                    s_dim = int(e_vec.get("dim", col_dim))
                    raw_vb = SparseVector(
                        s_dim,
                        [int(i) for i in e_vec["indices"]],
                        [float(v) for v in e_vec["values"]],
                    ).to_vectorbytes()
                else:
                    raw_vb = SparseVector(
                        col_dim, {int(k): float(v) for k, v in e_vec.items()}
                    ).to_vectorbytes()
            else:
                raise TypeError(
                    f"Cannot encode {type(e_vec).__name__} to VectorBytes for sparsevec column"
                )
        else:
            if isinstance(e_vec, (Vector, HalfVector, Bit, SparseVector)):
                raw_vb = e_vec.to_vectorbytes()
            elif isinstance(e_vec, (list, tuple)):
                raw_vb = encode_vector([float(x) for x in e_vec])
            else:
                raise ValueError(f"Cannot serialize vector without known column type {col_type!r}")
    else:
        raise ValueError("Entity must provide either vector or vector_bytes")

    res: List[Tuple[str, int, bytes]] = [
        DirectAttribute("arkiv.schema", 0x88, schema_tag.encode("utf-8")),
        DirectAttribute("av.vec", 0x07, raw_vb),
    ]

    if e_id is not None:
        res.append(DirectAttribute("av.id", 0x88, str(e_id).encode("utf-8")))

    if e_doc is not None:
        res.append(DirectAttribute("av.doc", 0x07, str(e_doc).encode("utf-8")))

    for attr_name in sorted(e_attrs.keys()):
        if attr_name == "arkiv.schema" or attr_name.startswith("av."):
            continue
        declared = col_attrs.get(attr_name)
        type_id, val_bytes = _serialize_attribute_value(e_attrs[attr_name], declared)
        res.append(DirectAttribute(attr_name, type_id, val_bytes))

    return res
