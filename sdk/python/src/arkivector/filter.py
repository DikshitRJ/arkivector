"""Arkivector filter AST builder (class F)."""

from typing import Any, Dict, List, Sequence

from arkivector.models import VALID_ATTR_TYPES, T, TypedValue


def _normalize_value(v: Any) -> Dict[str, Any]:
    """Normalize a filter predicate value into a wire TypedValue dict.

    Per filters.md §2:
    - TypedValue is preserved as-is.
    - bool -> T.bool(v)
    - int -> T.i32(v)
    - str -> T.str(v)
    - float -> raises TypeError because floats are ambiguous in Arkiv (must use T.dec explicitly).
    """
    if isinstance(v, TypedValue):
        return v.to_json()
    if isinstance(v, bool):
        return T.bool(v).to_json()
    if isinstance(v, int):
        return T.i32(v).to_json()
    if isinstance(v, float):
        raise TypeError(
            f"Floating-point value {v!r} is ambiguous in Arkiv (could be dec, halfvec, or vector). "
            "Use T.dec(...) explicitly for decimal attributes."
        )
    if isinstance(v, str):
        return T.str(v).to_json()
    if isinstance(v, dict) and "type" in v and "value" in v:
        return {"type": str(v["type"]), "value": v["value"]}
    raise TypeError(
        f"Unsupported value type for filter: {type(v).__name__}. "
        "Expected TypedValue, str, bool, or int."
    )


def _child_to_dict(child: Any) -> Dict[str, Any]:
    if isinstance(child, F):
        return child.to_dict()
    if hasattr(child, "to_dict") and callable(child.to_dict):
        res = child.to_dict()
        if isinstance(res, dict):
            return dict(res)
    if isinstance(child, dict):
        return dict(child)
    raise TypeError(f"Invalid filter child: {child!r}. Expected F instance or dict.")


def _flatten_children(args: Sequence[Any]) -> List[Any]:
    if len(args) == 1 and isinstance(args[0], (list, tuple)):
        return list(args[0])
    return list(args)


class F:
    """Filter builder producing exact Arkivector JSON ASTs per filters.md §2."""

    def __init__(self, data: Dict[str, Any]) -> None:
        self._data = data

    def to_dict(self) -> Dict[str, Any]:
        """Return the exact JSON AST dictionary per filters.md §2."""
        return dict(self._data)

    @classmethod
    def and_(cls, *children: Any) -> "F":
        """Conjunction of one or more filters (and: [Filter, ...])."""
        items = _flatten_children(children)
        if not items:
            raise ValueError("and_ requires at least one child filter")
        return cls({"and": [_child_to_dict(c) for c in items]})

    @classmethod
    def or_(cls, *children: Any) -> "F":
        """Disjunction of one or more filters (or: [Filter, ...])."""
        items = _flatten_children(children)
        if not items:
            raise ValueError("or_ requires at least one child filter")
        return cls({"or": [_child_to_dict(c) for c in items]})

    @classmethod
    def not_(cls, child: Any) -> "F":
        """Negation of a filter (not: Filter)."""
        return cls({"not": _child_to_dict(child)})

    @classmethod
    def eq(cls, field: str, value: Any) -> "F":
        """Equality predicate: field == value."""
        return cls({"eq": {str(field): _normalize_value(value)}})

    @classmethod
    def ne(cls, field: str, value: Any) -> "F":
        """Inequality predicate: field != value."""
        return cls({"ne": {str(field): _normalize_value(value)}})

    @classmethod
    def lt(cls, field: str, value: Any) -> "F":
        """Less-than predicate: field < value."""
        return cls({"lt": {str(field): _normalize_value(value)}})

    @classmethod
    def lte(cls, field: str, value: Any) -> "F":
        """Less-than-or-equal predicate: field <= value."""
        return cls({"lte": {str(field): _normalize_value(value)}})

    @classmethod
    def gt(cls, field: str, value: Any) -> "F":
        """Greater-than predicate: field > value."""
        return cls({"gt": {str(field): _normalize_value(value)}})

    @classmethod
    def gte(cls, field: str, value: Any) -> "F":
        """Greater-than-or-equal predicate: field >= value."""
        return cls({"gte": {str(field): _normalize_value(value)}})

    @classmethod
    def between(cls, field: str, low: Any, high: Any) -> "F":
        """Inclusive range predicate: field between low and high."""
        return cls({"between": {str(field): [_normalize_value(low), _normalize_value(high)]}})

    @classmethod
    def startswith(cls, field: str, prefix: str) -> "F":
        """Prefix match predicate on str fields: field startswith prefix."""
        if not isinstance(prefix, str):
            raise TypeError(f"prefix must be a str, got {type(prefix).__name__}")
        return cls({"startswith": {str(field): str(prefix)}})

    @classmethod
    def exists(cls, field: str) -> "F":
        """Field existence predicate."""
        return cls({"exists": str(field)})

    @classmethod
    def typeof(cls, field: str, attr_type: str) -> "F":
        """Field attribute type check predicate."""
        if attr_type not in VALID_ATTR_TYPES:
            raise ValueError(
                f"Invalid attribute type {attr_type!r}. Must be one of {sorted(VALID_ATTR_TYPES)}"
            )
        return cls({"typeof": {str(field): str(attr_type)}})

    def __and__(self, other: Any) -> "F":
        return F.and_(self, other)

    def __or__(self, other: Any) -> "F":
        return F.or_(self, other)

    def __invert__(self) -> "F":
        return F.not_(self)

    def __eq__(self, other: object) -> bool:
        if isinstance(other, F):
            return self._data == other._data
        if isinstance(other, dict):
            return self._data == other
        return NotImplemented

    def __repr__(self) -> str:
        return f"F({self._data!r})"
