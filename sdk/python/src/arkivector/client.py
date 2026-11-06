"""Arkivector API client."""

import base64
import os
import re
import threading
import urllib.parse
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
    Entity,
    IndexDef,
    Readiness,
    ScrollPage,
    SearchResult,
    SyncStatus,
    Ttl,
    VersionInfo,
)
from arkivector.types import Bit, HalfVector, SparseVector, Vector

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
    ) -> None:
        raw_url = url or os.environ.get("ARKIVECTOR_URL") or "http://127.0.0.1:8080"
        self.url = raw_url.rstrip("/")
        self.api_key = api_key or os.environ.get("ARKIVECTOR_API_KEY")
        self.timeout = float(timeout)
        self.max_retries = int(max_retries)
        self.session_consistency = bool(session_consistency)
        self.last_written_block: Optional[int] = None

        self._version_info: Optional[VersionInfo] = None
        self._collection_column_types: Dict[str, str] = {}
        self._lock = threading.Lock()

        self._http = HttpTransport(
            base_url=self.url,
            api_key=self.api_key,
            timeout=self.timeout,
            max_retries=self.max_retries,
        )

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
        if min_block is None and self.session_consistency and self.last_written_block is not None:
            min_block = self.last_written_block
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
            elif self.session_consistency and self.last_written_block is not None:
                consistency = Consistency(min_block=self.last_written_block)

        if consistency is not None:
            body["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request("POST", f"/v1/collections/{encoded_name}/search", body=body)
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
        if consistency is None and self.session_consistency and self.last_written_block is not None:
            consistency = Consistency(min_block=self.last_written_block)

        if consistency is not None:
            payload["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request(
            "POST", f"/v1/collections/{encoded_name}/search:batch", body=payload
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
            elif self.session_consistency and self.last_written_block is not None:
                consistency = Consistency(min_block=self.last_written_block)

        if consistency is not None:
            body["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request("POST", f"/v1/collections/{encoded_name}/scroll", body=body)
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
            elif self.session_consistency and self.last_written_block is not None:
                consistency = Consistency(min_block=self.last_written_block)

        if consistency is not None:
            body["consistency"] = (
                consistency.to_json() if isinstance(consistency, Consistency) else consistency
            )
            self.check_capability("consistency.min_block")

        encoded_name = urllib.parse.quote(collection, safe="")
        resp = self._http.request("POST", f"/v1/collections/{encoded_name}/count", body=body)
        data = resp.json() or {}
        return CountResult.from_json(data)

    def close(self) -> None:
        """Close client connections."""
        self._http.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.close()
