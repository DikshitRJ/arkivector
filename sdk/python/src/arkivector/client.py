"""Arkivector API client."""

import os
import threading
import urllib.parse
from typing import Any, Dict, List, Optional, Tuple, Union

from arkivector._http import HttpTransport
from arkivector.errors import ApiError, NotImplementedByDaemon
from arkivector.models import (
    Collection,
    ColumnDef,
    IndexDef,
    Readiness,
    SyncStatus,
    Ttl,
    VersionInfo,
)

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

    def close(self) -> None:
        """Close client connections."""
        self._http.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.close()
