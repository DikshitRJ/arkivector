"""Arkivector data models: TypedValue, T, Ttl, and API schemas."""

import base64
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Union

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
