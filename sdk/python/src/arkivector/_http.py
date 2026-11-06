"""Thread-safe HTTP transport with retry engine and problem error mapping."""

import http.client
import json
import random
import socket
import threading
import time
import urllib.parse
import uuid
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple, Union

from arkivector.errors import ApiError, TransportError, error_from_problem

SDK_VERSION = "0.1.0"
USER_AGENT = f"arkivector-python/{SDK_VERSION}"


@dataclass
class HttpResponse:
    """HTTP response container carrying status, headers, and body."""

    status: int
    headers: Dict[str, str]
    body: bytes
    request_id: str

    def json(self) -> Any:
        """Parse response body as JSON."""
        if not self.body:
            return None
        return json.loads(self.body.decode("utf-8"))

    @property
    def as_of_block(self) -> Optional[int]:
        """Block number reported by the Arkivector daemon."""
        val = self.headers.get("arkivector-as-of-block")
        return int(val) if val is not None else None


class ConnectionPool:
    """Thread-safe HTTP/HTTPS connection pool."""

    def __init__(self, idle_timeout: float = 4.0) -> None:
        self._idle_timeout = idle_timeout
        self._pool: Dict[
            Tuple[str, str, int],
            List[
                Tuple[
                    Union[http.client.HTTPConnection, http.client.HTTPSConnection],
                    float,
                ]
            ],
        ] = {}
        self._lock = threading.Lock()

    def acquire(
        self,
        scheme: str,
        host: str,
        port: int,
        timeout: float,
    ) -> Tuple[Union[http.client.HTTPConnection, http.client.HTTPSConnection], bool]:
        """Acquire a connection from the pool or create a new one. Returns (conn, is_reused)."""
        key = (scheme, host, port)
        now = time.monotonic()
        with self._lock:
            conns = self._pool.get(key)
            while conns:
                conn, last_used = conns.pop()
                if now - last_used < self._idle_timeout:
                    return conn, True
                else:
                    try:
                        conn.close()
                    except Exception:
                        pass

        if scheme == "https":
            return http.client.HTTPSConnection(host, port, timeout=timeout), False
        return http.client.HTTPConnection(host, port, timeout=timeout), False

    def release(
        self,
        scheme: str,
        host: str,
        port: int,
        conn: Union[http.client.HTTPConnection, http.client.HTTPSConnection],
        should_keep_alive: bool,
    ) -> None:
        """Return a connection to the pool or close it."""
        if not should_keep_alive:
            try:
                conn.close()
            except Exception:
                pass
            return

        key = (scheme, host, port)
        with self._lock:
            conns = self._pool.setdefault(key, [])
            conns.append((conn, time.monotonic()))

    def close(self) -> None:
        """Close all pooled connections."""
        with self._lock:
            for conns in self._pool.values():
                for conn, _ in conns:
                    try:
                        conn.close()
                    except Exception:
                        pass
            self._pool.clear()


class HttpTransport:
    """Thread-safe HTTP transport with retry engine."""

    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8080",
        api_key: Optional[str] = None,
        timeout: float = 10.0,
        max_retries: int = 3,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout
        self.max_retries = max_retries

        parsed = urllib.parse.urlsplit(self.base_url)
        self.scheme = parsed.scheme.lower()
        if self.scheme not in ("http", "https"):
            raise ValueError(f"Invalid URL scheme: {self.scheme!r}")
        self.host = parsed.hostname or "127.0.0.1"
        self.port = parsed.port or (443 if self.scheme == "https" else 80)
        self.base_path = parsed.path.rstrip("/")
        self._pool = ConnectionPool()

    def _send_once(
        self,
        method: str,
        path: str,
        headers: Dict[str, str],
        body_bytes: Optional[bytes],
        timeout: float,
    ) -> HttpResponse:
        conn, is_reused = self._pool.acquire(self.scheme, self.host, self.port, timeout=timeout)
        try:
            conn.request(method, path, body=body_bytes, headers=headers)
            resp = conn.getresponse()
            status = resp.status
            resp_headers = {k.lower(): v for k, v in resp.getheaders()}
            resp_body = resp.read()

            will_close = (
                getattr(resp, "will_close", False)
                or resp_headers.get("connection", "").lower() == "close"
            )
            self._pool.release(
                self.scheme, self.host, self.port, conn, should_keep_alive=not will_close
            )

            req_id = resp_headers.get("x-request-id") or headers.get("X-Request-Id", "")
            return HttpResponse(
                status=status,
                headers=resp_headers,
                body=resp_body,
                request_id=req_id,
            )
        except (http.client.RemoteDisconnected, BrokenPipeError, ConnectionResetError) as e:
            try:
                conn.close()
            except Exception:
                pass
            if is_reused:
                conn_fresh, _ = self._pool.acquire(
                    self.scheme, self.host, self.port, timeout=timeout
                )
                try:
                    conn_fresh.request(method, path, body=body_bytes, headers=headers)
                    resp = conn_fresh.getresponse()
                    status = resp.status
                    resp_headers = {k.lower(): v for k, v in resp.getheaders()}
                    resp_body = resp.read()
                    will_close = (
                        getattr(resp, "will_close", False)
                        or resp_headers.get("connection", "").lower() == "close"
                    )
                    self._pool.release(
                        self.scheme,
                        self.host,
                        self.port,
                        conn_fresh,
                        should_keep_alive=not will_close,
                    )
                    req_id = resp_headers.get("x-request-id") or headers.get("X-Request-Id", "")
                    return HttpResponse(
                        status=status,
                        headers=resp_headers,
                        body=resp_body,
                        request_id=req_id,
                    )
                except Exception as inner_e:
                    try:
                        conn_fresh.close()
                    except Exception:
                        pass
                    raise inner_e
            raise e
        except Exception:
            try:
                conn.close()
            except Exception:
                pass
            raise

    def can_retry(
        self,
        method: str,
        path: str,
        is_idempotent: bool = False,
        idempotency_key: Optional[str] = None,
    ) -> bool:
        """Determine if a request is eligible for automated retries per conventions.md §8."""
        is_idempotent_path = any(
            path.endswith(p) or f"{p}?" in path
            for p in (
                "/search",
                "/search:batch",
                "/scroll",
                "/count",
                "/entities:prepare",
            )
        )
        return bool(
            is_idempotent
            or is_idempotent_path
            or (method.upper() == "GET")
            or (idempotency_key is not None)
        )

    def request(
        self,
        method: str,
        path: str,
        *,
        body: Optional[Any] = None,
        query: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        request_id: Optional[str] = None,
        idempotency_key: Optional[str] = None,
        is_idempotent: bool = False,
        timeout: Optional[float] = None,
        auth: bool = True,
    ) -> HttpResponse:
        """Execute an HTTP request with headers, idempotency, and retries."""
        clean_path = path if path.startswith("/") else f"/{path}"
        full_path = f"{self.base_path}{clean_path}"
        if query:
            q_params = [(k, str(v)) for k, v in query.items() if v is not None]
            if q_params:
                full_path = f"{full_path}?{urllib.parse.urlencode(q_params)}"

        if body is not None:
            if isinstance(body, (bytes, bytearray)):
                body_bytes = bytes(body)
            elif isinstance(body, str):
                body_bytes = body.encode("utf-8")
            else:
                body_bytes = json.dumps(body).encode("utf-8")
        else:
            body_bytes = None

        req_headers: Dict[str, str] = {
            "User-Agent": USER_AGENT,
            "Accept": "application/json, application/problem+json",
        }
        req_id = request_id or uuid.uuid4().hex
        req_headers["X-Request-Id"] = req_id

        if auth and self.api_key:
            req_headers["Authorization"] = f"Bearer {self.api_key}"

        if body_bytes is not None:
            req_headers["Content-Type"] = "application/json"
            req_headers["Content-Length"] = str(len(body_bytes))

        if idempotency_key is not None:
            req_headers["Idempotency-Key"] = str(idempotency_key)

        if headers:
            req_headers.update(headers)

        is_idempotent_path = any(
            path.endswith(p) or f"{p}?" in path
            for p in (
                "/search",
                "/search:batch",
                "/scroll",
                "/count",
                "/entities:prepare",
            )
        )
        can_retry = (
            is_idempotent
            or is_idempotent_path
            or (method.upper() == "GET")
            or (idempotency_key is not None)
        )
        effective_timeout = timeout if timeout is not None else self.timeout
        attempts_limit = self.max_retries if can_retry else 0

        last_retry_after: Optional[float] = None

        for attempt in range(attempts_limit + 1):
            if attempt > 0:
                if last_retry_after is not None:
                    delay = last_retry_after
                else:
                    n = attempt - 1
                    base_delay = 0.2 * (2.0**n)
                    jitter = random.uniform(0.8, 1.2)
                    delay = base_delay * jitter
                time.sleep(delay)

            try:
                resp = self._send_once(
                    method=method.upper(),
                    path=full_path,
                    headers=req_headers,
                    body_bytes=body_bytes,
                    timeout=effective_timeout,
                )
            except (http.client.HTTPException, socket.error, OSError, TimeoutError) as e:
                trans_err = TransportError(str(e), request_id=req_id, cause=e)
                if attempt < attempts_limit:
                    last_retry_after = None
                    continue
                raise trans_err from e

            status = resp.status
            if 200 <= status < 300:
                return resp

            if clean_path == "/readyz" and status == 503:
                problem_data = self._parse_json(resp.body)
                if isinstance(problem_data, dict) and "ready" in problem_data:
                    return resp

            problem_dict = self._parse_json(resp.body)
            err = self._map_error(
                status=status,
                problem=problem_dict,
                request_id=resp.request_id,
                resp_body=resp.body,
            )

            if attempt < attempts_limit and err.retryable:
                retry_after_str = resp.headers.get("retry-after")
                last_retry_after = self._parse_retry_after(retry_after_str)
                continue

            raise err

        raise TransportError("Request failed after all retries", request_id=req_id)

    def _parse_json(self, body: bytes) -> Optional[Dict[str, Any]]:
        if not body:
            return None
        try:
            data = json.loads(body.decode("utf-8"))
            if isinstance(data, dict):
                return data
            return None
        except Exception:
            return None

    def _map_error(
        self,
        status: int,
        problem: Optional[Dict[str, Any]],
        request_id: str,
        resp_body: bytes,
    ) -> ApiError:
        if problem and ("code" in problem or "type" in problem):
            return error_from_problem(problem, status=status, request_id=request_id)

        retryable = status in (429, 502, 503, 504)
        detail = resp_body.decode("utf-8", errors="replace") if resp_body else None
        code = (
            "rate_limited"
            if status == 429
            else ("not_ready" if status == 503 else f"http_{status}")
        )
        synth_problem: Dict[str, Any] = {
            "type": f"urn:arkivector:error:{code}",
            "title": f"HTTP {status}",
            "status": status,
            "code": code,
            "retryable": retryable,
            "detail": detail,
            "request_id": request_id,
        }
        return error_from_problem(synth_problem, status=status, request_id=request_id)

    @staticmethod
    def _parse_retry_after(header_val: Optional[str]) -> Optional[float]:
        if not header_val:
            return None
        try:
            val = float(header_val.strip())
            return max(0.0, val)
        except ValueError:
            return None

    def close(self) -> None:
        """Close connection pool."""
        self._pool.close()
