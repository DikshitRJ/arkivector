"""Arkivector Python SDK."""

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
from arkivector.models import T, Ttl, TypedValue
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
    "Bit",
    "ConflictError",
    "HalfVector",
    "NotFoundError",
    "NotImplementedByDaemon",
    "SparseVector",
    "T",
    "TransportError",
    "Ttl",
    "TypedValue",
    "UnavailableError",
    "ValidationError",
    "Vector",
    "VectorBytesError",
    "error_from_problem",
    "vectorbytes_decode_any",
]
