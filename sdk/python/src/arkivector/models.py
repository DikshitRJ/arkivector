"""Arkivector data models: TypedValue, T constructor helper, and Ttl."""

import base64
from dataclasses import dataclass
from typing import Any, Dict, Union

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
