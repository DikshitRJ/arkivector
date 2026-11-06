"""Arkivector data models: TypedValue, T, Ttl, and API schemas."""

import base64
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Dict, List, Optional, Union

from arkivector.vectorbytes import vectorbytes_decode_any

if TYPE_CHECKING:
    from arkivector.types import AnyVector

_bool = bool
_int = int
_float = float
_str = str
_bytes = bytes

VALID_ATTR_TYPES = {
    "bool",
    "i32",
    "u64",
    "u256",
    "dec",
    "str",
    "addr",
    "key",
    "bytes",
}


@dataclass(frozen=True)
class TypedValue:
    """Arkivector canonical typed attribute value."""

    type: str
    value: Any

    def __post_init__(self) -> None:
        if self.type not in VALID_ATTR_TYPES:
            raise ValueError(
                f"Invalid attribute type {self.type!r}. Must be one of {sorted(VALID_ATTR_TYPES)}"
            )

    def as_str(self) -> str:
        """Return value as string."""
        return str(self.value)

    def as_dec_string(self) -> str:
        """Return decimal value as string."""
        return str(self.value)

    def as_i64(self) -> int:
        """Return value as 64-bit integer."""
        return int(self.value)

    def as_int(self) -> int:
        """Alias for as_i64."""
        return int(self.value)

    def as_bool(self) -> bool:
        """Return value as boolean."""
        if isinstance(self.value, bool):
            return self.value
        if isinstance(self.value, str):
            return self.value.lower() in ("true", "1")
        return bool(self.value)

    def as_bytes(self) -> bytes:
        """Decode base64 bytes value."""
        if isinstance(self.value, (bytes, bytearray, memoryview)):
            return bytes(self.value)
        return base64.b64decode(str(self.value))

    def as_float(self) -> float:
        """Return value as float."""
        return float(self.value)

    def plain(self) -> Any:
        """Return the native Python scalar (dec, u64, u256 stay strings)."""
        if self.type == "bool":
            return self.as_bool()
        if self.type == "i32":
            return self.as_i64()
        if self.type == "bytes":
            return self.as_bytes()
        return str(self.value)

    def to_json(self) -> Dict[str, Any]:
        """Serialize to wire JSON dict."""
        return {"type": self.type, "value": self.value}

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "TypedValue":
        """Deserialize from wire JSON dict."""
        return cls(type=str(d["type"]), value=d["value"])


class T:
    """Helper factory for constructing canonical TypedValue instances."""

    @staticmethod
    def bool(val: _bool) -> TypedValue:
        return TypedValue("bool", _bool(val))

    @staticmethod
    def i32(val: _int) -> TypedValue:
        return TypedValue("i32", _int(val))

    @staticmethod
    def u64(val: Union[_int, _str]) -> TypedValue:
        return TypedValue("u64", _str(val))

    @staticmethod
    def u256(val: Union[_int, _str]) -> TypedValue:
        return TypedValue("u256", _str(val))

    @staticmethod
    def dec(val: Union[_float, _int, _str]) -> TypedValue:
        return TypedValue("dec", _str(val))

    @staticmethod
    def str(val: _str) -> TypedValue:
        return TypedValue("str", _str(val))

    @staticmethod
    def addr(val: _str) -> TypedValue:
        return TypedValue("addr", _str(val))

    @staticmethod
    def key(val: _str) -> TypedValue:
        return TypedValue("key", _str(val))

    @staticmethod
    def bytes(val: Union[_bytes, bytearray, memoryview, _str]) -> TypedValue:
        if isinstance(val, (_bytes, bytearray, memoryview)):
            b64_val = base64.b64encode(_bytes(val)).decode("ascii")
            return TypedValue("bytes", b64_val)
        return TypedValue("bytes", _str(val))


class Ttl:
    """Entity lifetime specification for Arkiv."""

    def __init__(self, kind: str, value: Union[int, float]) -> None:
        self._kind = kind
        self._value = value

    @property
    def kind(self) -> str:
        """The active TTL key ('blocks', 'seconds', 'days', 'expires_at_block')."""
        return self._kind

    @property
    def value(self) -> Union[int, float]:
        """The TTL numeric value."""
        return self._value

    @classmethod
    def blocks(cls, n: int) -> "Ttl":
        """Relative lifetime in blocks from inclusion (1..7,884,000)."""
        val = int(n)
        if val < 1 or val > 7884000:
            raise ValueError(f"blocks out of range [1, 7884000]: {val}")
        return cls("blocks", val)

    @classmethod
    def seconds(cls, n: int) -> "Ttl":
        """Relative lifetime in seconds (must be even, 2..15,768,000)."""
        val = int(n)
        if val < 2 or val > 15768000:
            raise ValueError(f"seconds out of range [2, 15768000]: {val}")
        if val % 2 != 0:
            raise ValueError(f"seconds must be even (2 s blocks): {val}")
        return cls("seconds", val)

    @classmethod
    def days(cls, x: Union[float, int]) -> "Ttl":
        """Relative lifetime in days (0 < days <= 182.5)."""
        val = float(x) if isinstance(x, float) else int(x)
        if val <= 0 or val > 182.5:
            raise ValueError(f"days out of range (0, 182.5]: {val}")
        return cls("days", val)

    @classmethod
    def at_block(cls, n: int) -> "Ttl":
        """Absolute expiry block number."""
        val = int(n)
        if val < 0:
            raise ValueError(f"expires_at_block must be non-negative: {val}")
        return cls("expires_at_block", val)

    def to_json(self) -> Dict[str, Any]:
        """Serialize to wire JSON object with exactly one key."""
        return {self._kind: self._value}

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "Ttl":
        """Deserialize from wire JSON object with exactly one key."""
        if "blocks" in d:
            return cls.blocks(int(d["blocks"]))
        if "seconds" in d:
            return cls.seconds(int(d["seconds"]))
        if "days" in d:
            return cls.days(d["days"])
        if "expires_at_block" in d:
            return cls.at_block(int(d["expires_at_block"]))
        raise ValueError(f"Unknown Ttl payload: {d}")

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Ttl):
            return self._kind == other._kind and self._value == other._value
        return NotImplemented

    def __repr__(self) -> str:
        if self._kind == "expires_at_block":
            return f"Ttl.at_block({self._value})"
        return f"Ttl.{self._kind}({self._value})"


@dataclass
class VersionInfo:
    """Daemon and contract version info along with advertised capabilities."""

    daemon_version: str
    api_version: str
    pgvector_version: str
    kernel_tier: str
    chain_id: int
    writes_enabled: bool
    capabilities: List[str]

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "VersionInfo":
        return cls(
            daemon_version=str(d.get("daemon_version", "")),
            api_version=str(d.get("api_version", "")),
            pgvector_version=str(d.get("pgvector_version", "")),
            kernel_tier=str(d.get("kernel_tier", "")),
            chain_id=int(d.get("chain_id", 0)),
            writes_enabled=bool(d.get("writes_enabled", False)),
            capabilities=[str(c) for c in d.get("capabilities", [])],
        )

    def to_json(self) -> Dict[str, Any]:
        return {
            "daemon_version": self.daemon_version,
            "api_version": self.api_version,
            "pgvector_version": self.pgvector_version,
            "kernel_tier": self.kernel_tier,
            "chain_id": self.chain_id,
            "writes_enabled": self.writes_enabled,
            "capabilities": list(self.capabilities),
        }


@dataclass
class Readiness:
    """Readiness probe response indicating whether all collections are live."""

    ready: bool
    collections: Dict[str, str]

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "Readiness":
        cols = d.get("collections", {})
        return cls(
            ready=bool(d.get("ready", False)),
            collections={str(k): str(v) for k, v in cols.items()} if isinstance(cols, dict) else {},
        )

    def to_json(self) -> Dict[str, Any]:
        return {
            "ready": self.ready,
            "collections": dict(self.collections),
        }


@dataclass
class SyncStatus:
    """Sync status for a tracked collection."""

    collection: str
    chain_id: Optional[int] = None
    schema_tag: Optional[str] = None
    state: str = "init"
    head_block: Optional[int] = None
    indexed_block: Optional[int] = None
    as_of_block: Optional[int] = None
    last_sync_time: Optional[Union[int, float, str]] = None
    quarantine_count: int = 0
    error: Optional[str] = None
    lag_blocks: Optional[int] = None

    @property
    def name(self) -> str:
        """Alias for collection name matching wire schema."""
        return self.collection

    @property
    def last_error(self) -> Optional[str]:
        """Alias for error matching wire schema."""
        return self.error

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "SyncStatus":
        collection = str(d.get("collection") or d.get("name") or "")
        state = str(d.get("state", "init"))
        as_of_block = d.get("as_of_block")
        head_block = d.get("head_block")
        chain_id = d.get("chain_id")
        schema_tag = d.get("schema_tag")
        indexed_block = d.get("indexed_block")
        if indexed_block is None:
            indexed_block = as_of_block
        last_sync_time = d.get("last_sync_time")
        quarantine_count = int(d.get("quarantine_count", 0))
        error = d.get("error") if "error" in d else d.get("last_error")
        lag_blocks = d.get("lag_blocks")
        return cls(
            collection=collection,
            chain_id=int(chain_id) if chain_id is not None else None,
            schema_tag=str(schema_tag) if schema_tag is not None else None,
            state=state,
            head_block=int(head_block) if head_block is not None else None,
            indexed_block=int(indexed_block) if indexed_block is not None else None,
            as_of_block=int(as_of_block) if as_of_block is not None else None,
            last_sync_time=last_sync_time,
            quarantine_count=quarantine_count,
            error=str(error) if error is not None else None,
            lag_blocks=int(lag_blocks) if lag_blocks is not None else None,
        )

    def to_json(self) -> Dict[str, Any]:
        d: Dict[str, Any] = {
            "name": self.collection,
            "state": self.state,
        }
        if self.as_of_block is not None:
            d["as_of_block"] = self.as_of_block
        if self.head_block is not None:
            d["head_block"] = self.head_block
        if self.chain_id is not None:
            d["chain_id"] = self.chain_id
        if self.schema_tag is not None:
            d["schema_tag"] = self.schema_tag
        if self.lag_blocks is not None:
            d["lag_blocks"] = self.lag_blocks
        if self.error is not None:
            d["last_error"] = self.error
        return d


@dataclass
class ColumnDef:
    """Vector column specification (vector type and dimensionality)."""

    type: str
    dimensions: int

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "ColumnDef":
        return cls(type=str(d.get("type", "")), dimensions=int(d.get("dimensions", 0)))

    def to_json(self) -> Dict[str, Any]:
        return {"type": self.type, "dimensions": self.dimensions}


Column = ColumnDef


@dataclass
class IndexDef:
    """Index configuration (method, operator class, and parameters)."""

    method: str
    opclass: str
    with_params: Dict[str, Any] = field(default_factory=dict)

    @property
    def with_(self) -> Dict[str, Any]:
        """Access index with parameters."""
        return self.with_params

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "IndexDef":
        method = str(d.get("method", "hnsw"))
        opclass = str(d.get("opclass", ""))
        raw_with = d.get("with") or d.get("with_params") or {}
        with_params = dict(raw_with) if isinstance(raw_with, dict) else {}
        return cls(method=method, opclass=opclass, with_params=with_params)

    def to_json(self) -> Dict[str, Any]:
        res: Dict[str, Any] = {
            "method": self.method,
            "opclass": self.opclass,
        }
        if self.with_params:
            res["with"] = self.with_params
        return res


@dataclass
class Collection:
    """Arkivector collection definition and local index state."""

    name: str
    schema_tag: str
    column: ColumnDef
    index: IndexDef
    writers: List[str]
    membership: str
    attributes: Dict[str, str]
    default_ttl: Optional[Ttl]
    open: bool
    manifest_key: Optional[str]
    stats: Dict[str, Any]
    sync: Optional[SyncStatus] = None
    mode: str = "full"
    model: Optional[str] = None

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "Collection":
        name = str(d.get("name", ""))
        schema_tag = str(d.get("schema_tag", f"arkivector.v1.{name}"))
        col_raw = d.get("column", {})
        column = (
            ColumnDef.from_json(col_raw)
            if isinstance(col_raw, dict)
            else (col_raw if isinstance(col_raw, ColumnDef) else ColumnDef("", 0))
        )
        idx_raw = d.get("index", {})
        index = (
            IndexDef.from_json(idx_raw)
            if isinstance(idx_raw, dict)
            else (idx_raw if isinstance(idx_raw, IndexDef) else IndexDef("none", ""))
        )
        writers = [str(w) for w in d.get("writers", [])]
        membership = str(d.get("membership", "creator"))
        raw_attrs = d.get("attributes", {})
        attributes = (
            {str(k): str(v) for k, v in raw_attrs.items()} if isinstance(raw_attrs, dict) else {}
        )
        raw_ttl = d.get("default_ttl")
        default_ttl: Optional[Ttl] = None
        if isinstance(raw_ttl, dict):
            default_ttl = Ttl.from_json(raw_ttl)
        elif isinstance(raw_ttl, Ttl):
            default_ttl = raw_ttl
        open_flag = bool(d.get("open", membership == "open"))
        manifest_key = d.get("manifest_key")
        raw_stats = d.get("stats", {})
        stats = dict(raw_stats) if isinstance(raw_stats, dict) else {}
        sync_raw = d.get("sync")
        sync = SyncStatus.from_json(sync_raw) if isinstance(sync_raw, dict) else None
        mode = str(d.get("mode", "full"))
        model = str(d.get("model")) if d.get("model") is not None else None
        return cls(
            name=name,
            schema_tag=schema_tag,
            column=column,
            index=index,
            writers=writers,
            membership=membership,
            attributes=attributes,
            default_ttl=default_ttl,
            open=open_flag,
            manifest_key=str(manifest_key) if manifest_key is not None else None,
            stats=stats,
            sync=sync,
            mode=mode,
            model=model,
        )

    def to_json(self) -> Dict[str, Any]:
        res: Dict[str, Any] = {
            "name": self.name,
            "schema_tag": self.schema_tag,
            "column": self.column.to_json(),
            "index": self.index.to_json(),
            "writers": list(self.writers),
            "membership": self.membership,
            "attributes": dict(self.attributes),
            "default_ttl": self.default_ttl.to_json() if self.default_ttl else None,
            "open": self.open,
            "manifest_key": self.manifest_key,
            "stats": dict(self.stats),
            "mode": self.mode,
        }
        if self.sync is not None:
            res["sync"] = self.sync.to_json()
        if self.model is not None:
            res["model"] = self.model
        return res


@dataclass
class Consistency:
    """Read-your-writes consistency specification (min_block and timeout_ms)."""

    min_block: Optional[int] = None
    timeout_ms: Optional[int] = None

    def to_json(self) -> Dict[str, Any]:
        d: Dict[str, Any] = {}
        if self.min_block is not None:
            d["min_block"] = int(self.min_block)
        if self.timeout_ms is not None:
            d["timeout_ms"] = int(self.timeout_ms)
        return d

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "Consistency":
        return cls(
            min_block=int(d["min_block"]) if d.get("min_block") is not None else None,
            timeout_ms=int(d["timeout_ms"]) if d.get("timeout_ms") is not None else None,
        )


@dataclass
class Entity:
    """Arkivector collection entity."""

    key: str
    id: Optional[str] = None
    owner: str = ""
    creator: str = ""
    created_at_block: int = 0
    updated_at_block: int = 0
    expires_at_block: int = 0
    attributes: Dict[str, TypedValue] = field(default_factory=dict)
    document: Optional[str] = None
    vector_bytes: Optional[str] = None
    vector: Optional["AnyVector"] = None

    def __post_init__(self) -> None:
        if self.vector is None and self.vector_bytes:
            try:
                self.vector = vectorbytes_decode_any(base64.b64decode(self.vector_bytes))
            except Exception:
                pass

    def plain_attributes(self) -> Dict[str, Any]:
        """Return a mapping of attribute names to native Python scalars."""
        return {
            k: v.plain() if isinstance(v, TypedValue) else v for k, v in self.attributes.items()
        }

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "Entity":
        raw_attrs = d.get("attributes", {})
        attributes: Dict[str, TypedValue] = {}
        if isinstance(raw_attrs, dict):
            for k, v in raw_attrs.items():
                if isinstance(v, TypedValue):
                    attributes[str(k)] = v
                elif isinstance(v, dict) and "type" in v and "value" in v:
                    attributes[str(k)] = TypedValue.from_json(v)
                elif isinstance(v, bool):
                    attributes[str(k)] = T.bool(v)
                elif isinstance(v, int):
                    attributes[str(k)] = T.i32(v)
                elif isinstance(v, str):
                    attributes[str(k)] = T.str(v)
                else:
                    attributes[str(k)] = TypedValue("str", str(v))
        return cls(
            key=str(d.get("key", "")),
            id=str(d["id"]) if d.get("id") is not None else None,
            owner=str(d.get("owner", "")),
            creator=str(d.get("creator", "")),
            created_at_block=int(d.get("created_at_block", 0)),
            updated_at_block=int(d.get("updated_at_block", 0)),
            expires_at_block=int(d.get("expires_at_block", 0)),
            attributes=attributes,
            document=str(d["document"]) if d.get("document") is not None else None,
            vector_bytes=str(d["vector_bytes"]) if d.get("vector_bytes") is not None else None,
        )

    def to_json(self) -> Dict[str, Any]:
        res: Dict[str, Any] = {
            "key": self.key,
            "id": self.id,
            "owner": self.owner,
            "creator": self.creator,
            "created_at_block": self.created_at_block,
            "updated_at_block": self.updated_at_block,
            "expires_at_block": self.expires_at_block,
            "attributes": {k: v.to_json() for k, v in self.attributes.items()},
        }
        if self.document is not None:
            res["document"] = self.document
        if self.vector_bytes is not None:
            res["vector_bytes"] = self.vector_bytes
        return res


@dataclass
class SearchHit:
    """A single vector similarity search result hit."""

    key: str
    id: Optional[str]
    distance: float
    score: float
    owner: str
    creator: str
    expires_at_block: int
    attributes: Dict[str, TypedValue] = field(default_factory=dict)
    document: Optional[str] = None
    vector_bytes: Optional[str] = None
    vector: Optional["AnyVector"] = None

    def __post_init__(self) -> None:
        if self.vector is None and self.vector_bytes:
            try:
                self.vector = vectorbytes_decode_any(base64.b64decode(self.vector_bytes))
            except Exception:
                pass

    def plain_attributes(self) -> Dict[str, Any]:
        """Return a mapping of attribute names to native Python scalars."""
        return {
            k: v.plain() if isinstance(v, TypedValue) else v for k, v in self.attributes.items()
        }

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "SearchHit":
        raw_attrs = d.get("attributes", {})
        attributes: Dict[str, TypedValue] = {}
        if isinstance(raw_attrs, dict):
            for k, v in raw_attrs.items():
                if isinstance(v, TypedValue):
                    attributes[str(k)] = v
                elif isinstance(v, dict) and "type" in v and "value" in v:
                    attributes[str(k)] = TypedValue.from_json(v)
                elif isinstance(v, bool):
                    attributes[str(k)] = T.bool(v)
                elif isinstance(v, int):
                    attributes[str(k)] = T.i32(v)
                elif isinstance(v, str):
                    attributes[str(k)] = T.str(v)
                else:
                    attributes[str(k)] = TypedValue("str", str(v))
        return cls(
            key=str(d.get("key", "")),
            id=str(d["id"]) if d.get("id") is not None else None,
            distance=float(d.get("distance", 0.0)),
            score=float(d.get("score", 0.0)),
            owner=str(d.get("owner", "")),
            creator=str(d.get("creator", "")),
            expires_at_block=int(d.get("expires_at_block", 0)),
            attributes=attributes,
            document=str(d["document"]) if d.get("document") is not None else None,
            vector_bytes=str(d["vector_bytes"]) if d.get("vector_bytes") is not None else None,
        )

    def to_json(self) -> Dict[str, Any]:
        res: Dict[str, Any] = {
            "key": self.key,
            "id": self.id,
            "distance": self.distance,
            "score": self.score,
            "owner": self.owner,
            "creator": self.creator,
            "expires_at_block": self.expires_at_block,
            "attributes": {k: v.to_json() for k, v in self.attributes.items()},
        }
        if self.document is not None:
            res["document"] = self.document
        if self.vector_bytes is not None:
            res["vector_bytes"] = self.vector_bytes
        return res


@dataclass
class SearchResult:
    """Vector similarity search response."""

    as_of_block: int
    sync_state: str
    open_collection: bool
    hits: List[SearchHit]
    plan: Optional[Dict[str, Any]] = None

    @property
    def results(self) -> List[SearchHit]:
        """Alias for hits matching wire schema."""
        return self.hits

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "SearchResult":
        as_of_block = int(d.get("as_of_block", 0))
        sync_state = str(d.get("sync_state", "live"))
        open_collection = bool(d.get("open_collection", False))
        raw_hits = d.get("results") if "results" in d else d.get("hits", [])
        hits = [SearchHit.from_json(h) for h in (raw_hits or [])]
        plan = d.get("plan")
        return cls(
            as_of_block=as_of_block,
            sync_state=sync_state,
            open_collection=open_collection,
            hits=hits,
            plan=plan if isinstance(plan, dict) else None,
        )

    def to_json(self) -> Dict[str, Any]:
        res: Dict[str, Any] = {
            "as_of_block": self.as_of_block,
            "sync_state": self.sync_state,
            "open_collection": self.open_collection,
            "results": [h.to_json() for h in self.hits],
        }
        if self.plan is not None:
            res["plan"] = self.plan
        return res


@dataclass
class BatchResult:
    """Batch search response containing multiple search results."""

    as_of_block: int
    sync_state: str
    results: List[SearchResult]

    @property
    def responses(self) -> List[SearchResult]:
        """Alias for results matching wire schema."""
        return self.results

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "BatchResult":
        as_of_block = int(d.get("as_of_block", 0))
        sync_state = str(d.get("sync_state", "live"))
        raw_items = d.get("responses") if "responses" in d else d.get("results", [])
        results: List[SearchResult] = []
        for item in raw_items or []:
            if isinstance(item, SearchResult):
                results.append(item)
            elif isinstance(item, dict):
                raw_hits = item.get("results") if "results" in item else item.get("hits", [])
                hits = [SearchHit.from_json(h) for h in (raw_hits or [])]
                plan = item.get("plan")
                results.append(
                    SearchResult(
                        as_of_block=as_of_block,
                        sync_state=sync_state,
                        open_collection=bool(item.get("open_collection", False)),
                        hits=hits,
                        plan=plan if isinstance(plan, dict) else None,
                    )
                )
        return cls(
            as_of_block=as_of_block,
            sync_state=sync_state,
            results=results,
        )

    def to_json(self) -> Dict[str, Any]:
        return {
            "as_of_block": self.as_of_block,
            "sync_state": self.sync_state,
            "responses": [
                {"results": [h.to_json() for h in r.hits], **({"plan": r.plan} if r.plan else {})}
                for r in self.results
            ],
        }


@dataclass
class ScrollPage:
    """A page of entities returned by scroll."""

    entities: List[Entity]
    next_cursor: Optional[str]
    as_of_block: int

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "ScrollPage":
        as_of_block = int(d.get("as_of_block", 0))
        raw_entities = d.get("entities", [])
        entities = [Entity.from_json(e) for e in (raw_entities or [])]
        next_cursor = d.get("next_cursor")
        return cls(
            entities=entities,
            next_cursor=str(next_cursor) if next_cursor is not None else None,
            as_of_block=as_of_block,
        )

    def to_json(self) -> Dict[str, Any]:
        return {
            "as_of_block": self.as_of_block,
            "entities": [e.to_json() for e in self.entities],
            "next_cursor": self.next_cursor,
        }


@dataclass
class CountResult:
    """Count response of matching live entities."""

    count: int
    as_of_block: int

    @classmethod
    def from_json(cls, d: Dict[str, Any]) -> "CountResult":
        return cls(
            count=int(d.get("count", 0)),
            as_of_block=int(d.get("as_of_block", 0)),
        )

    def to_json(self) -> Dict[str, Any]:
        return {
            "as_of_block": self.as_of_block,
            "count": self.count,
        }
