"""Arkivector Python SDK."""

from arkivector.client import Client
from arkivector.errors import (
    ApiError,
    AuthError,
    ConflictError,
    NotFoundError,
    NotImplementedByDaemon,
    TransportError,
    UnavailableError,
    ValidationError,
    VectorBytesError,
    error_from_problem,
)
from arkivector.filter import F
from arkivector.models import (
    BatchResult,
    Collection,
    Column,
    ColumnDef,
    Consistency,
    CountResult,
    Entity,
    IndexDef,
    Readiness,
    ScrollPage,
    SearchHit,
    SearchResult,
    SyncStatus,
    T,
    Ttl,
    TypedValue,
    VersionInfo,
)
from arkivector.types import (
    AnyVector,
    Bit,
    HalfVector,
    SparseVector,
    Vector,
)
from arkivector.vectorbytes import vectorbytes_decode_any

__version__ = "0.1.0"
SUPPORTED_API_MAJOR = 1

__all__ = [
    "__version__",
    "SUPPORTED_API_MAJOR",
    "AnyVector",
    "ApiError",
    "AuthError",
    "BatchResult",
    "Bit",
    "Client",
    "Collection",
    "Column",
    "ColumnDef",
    "ConflictError",
    "Consistency",
    "CountResult",
    "Entity",
    "F",
    "HalfVector",
    "IndexDef",
    "NotFoundError",
    "NotImplementedByDaemon",
    "Readiness",
    "ScrollPage",
    "SearchHit",
    "SearchResult",
    "SparseVector",
    "SyncStatus",
    "T",
    "TransportError",
    "Ttl",
    "TypedValue",
    "UnavailableError",
    "ValidationError",
    "Vector",
    "VectorBytesError",
    "VersionInfo",
    "error_from_problem",
    "vectorbytes_decode_any",
]
