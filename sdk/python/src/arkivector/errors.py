"""Arkivector error types and hierarchy."""

from typing import Any, Dict, Optional


class VectorBytesError(ValueError):
    """Local codec error raised by VectorBytes encoder or decoder."""

    def __init__(self, code: str, detail: Optional[str] = None) -> None:
        self.code = code
        self.detail = detail
        msg = f"{code}: {detail}" if detail else code
        super().__init__(msg)

    def __repr__(self) -> str:
        if self.detail:
            return f"VectorBytesError({self.code!r}, detail={self.detail!r})"
        return f"VectorBytesError({self.code!r})"


class ApiError(Exception):
    """Base error for all daemon API responses and HTTP-derived errors."""

    def __init__(
        self,
        code: str,
        status: int = 0,
        title: str = "",
        detail: Optional[str] = None,
        retryable: bool = False,
        request_id: Optional[str] = None,
        problem: Optional[Dict[str, Any]] = None,
    ) -> None:
        self.code = code
        self.status = status
        self.title = title
        self.detail = detail
        self.retryable = retryable
        self.request_id = request_id
        self.problem = problem or {}

        summary = f"[{status}] {code}" if status else code
        if title:
            summary = f"{summary}: {title}"
        if detail:
            summary = f"{summary} - {detail}"
        super().__init__(summary)

    @property
    def field(self) -> Optional[str]:
        """JSON pointer into the request body if present."""
        val = self.problem.get("field")
        return str(val) if val is not None else None

    @property
    def position(self) -> Optional[int]:
        """Byte offset in 'where' query string if present."""
        val = self.problem.get("position")
        return int(val) if val is not None else None

    @property
    def vb_error(self) -> Optional[str]:
        """VB_* error code if vector validation failed."""
        val = self.problem.get("vb_error")
        return str(val) if val is not None else None

    @property
    def as_of_block(self) -> Optional[int]:
        """Current block number for consistency or not_ready errors."""
        val = self.problem.get("as_of_block")
        return int(val) if val is not None else None

    @property
    def revert_error(self) -> Optional[str]:
        """Solidity custom error name if tx reverted."""
        val = self.problem.get("revert_error")
        return str(val) if val is not None else None


class TransportError(ApiError):
    """Network, connection, or protocol failure before receiving an HTTP response."""

    def __init__(
        self,
        message: str = "Transport error",
        *,
        request_id: Optional[str] = None,
        cause: Optional[BaseException] = None,
    ) -> None:
        super().__init__(
            code="transport_error",
            status=0,
            title="Transport error",
            detail=message,
            retryable=True,
            request_id=request_id,
        )
        if cause is not None:
            self.__cause__ = cause


class AuthError(ApiError):
    """Authentication or authorization failure (unauthorized, forbidden, write_disabled)."""


class NotFoundError(ApiError):
    """Resource not found (collection_not_found, entity_not_found, route_not_found)."""


class ConflictError(ApiError):
    """Conflict error (collection_exists, not_owner, tx_reverted, idempotency_conflict)."""


class ValidationError(ApiError):
    """Validation or malformed input error (bad_request, invalid_*, dimension_mismatch, etc)."""


class UnavailableError(ApiError):
    """Service temporarily unavailable (rate_limited, arkiv_rpc_error, not_ready, timeout, etc)."""


class NotImplementedByDaemon(ApiError):
    """Feature not implemented or not enabled on this daemon build."""


def error_from_problem(
    problem: Dict[str, Any],
    status: Optional[int] = None,
    request_id: Optional[str] = None,
) -> ApiError:
    """Instantiate the appropriate typed ApiError subclass from an RFC 9457 problem dict."""
    code = str(problem.get("code", "unknown"))
    http_status = int(status or problem.get("status", 0))
    title = str(problem.get("title", ""))
    detail = problem.get("detail")
    detail_str = str(detail) if detail is not None else None
    retryable = bool(problem.get("retryable", False))
    req_id = request_id or (str(problem["request_id"]) if "request_id" in problem else None)

    target_cls = ApiError
    if code in ("unauthorized", "forbidden", "write_disabled"):
        target_cls = AuthError
    elif code.endswith("_not_found") or code == "not_found":
        target_cls = NotFoundError
    elif code in ("collection_exists", "not_owner", "tx_reverted", "idempotency_conflict"):
        target_cls = ConflictError
    elif code.startswith("invalid_") or code in (
        "bad_request",
        "dimension_mismatch",
        "payload_too_large",
        "unsupported_media_type",
    ):
        target_cls = ValidationError
    elif code in (
        "rate_limited",
        "arkiv_rpc_error",
        "not_ready",
        "timeout",
        "consistency_timeout",
    ):
        target_cls = UnavailableError
    elif code == "not_implemented":
        target_cls = NotImplementedByDaemon

    return target_cls(
        code=code,
        status=http_status,
        title=title,
        detail=detail_str,
        retryable=retryable,
        request_id=req_id,
        problem=problem,
    )
