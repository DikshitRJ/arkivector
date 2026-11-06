"""Shared fixtures for the Arkivector integration tests.

Fixtures
--------
mock_arkiv          function scope: a fresh mock-arkiv (``--port 0 --seed 7``) per test.
mock_arkiv_factory  function scope: ``factory(*extra_args, seed=7)`` starts more mocks (all killed at teardown).
daemon              PLACEHOLDER for phase D2+ (see the marked block below).

The mock is started with ``sys.executable``, so run pytest from a venv that has
``requirements.txt`` installed.
"""

from __future__ import annotations

import json
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest

HERE = Path(__file__).resolve().parent
DAEMON_DIR = HERE.parent.parent
MOCK_DIR = DAEMON_DIR / "testkit" / "mock-arkiv"
DAEMON_BIN = DAEMON_DIR / "build" / "dev" / "arkivectord"

CONTRACT = "0x4400000000000000000000000000000000000044"
DEFAULT_OWNER = "0x" + "11" * 20


class JsonRpcError(Exception):
    """A JSON-RPC error object returned by the server."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(f"[{code}] {message} {data!r}")
        self.code = code
        self.message = message
        self.data = data


class HttpStatusError(Exception):
    """RPC POST answered with a non-2xx HTTP status (e.g. 429)."""

    def __init__(self, status: int, body: Any, headers: Any) -> None:
        super().__init__(f"HTTP {status}: {body!r}")
        self.status = status
        self.body = body
        self.headers = headers


class MockArkiv:
    """Handle to a running mock-arkiv subprocess."""

    def __init__(self, proc: subprocess.Popen[str], port: int) -> None:
        self.proc = proc
        self.port = port
        self.url = f"http://127.0.0.1:{port}"
        self.ws_url = f"ws://127.0.0.1:{port}/"
        self._id = 0

    # -- raw HTTP ---------------------------------------------------------
    def http(self, method: str, path: str, body: Any = None) -> tuple[int, Any, Any]:
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(
            self.url + path, data=data, method=method, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read()
                return r.status, (json.loads(raw) if raw else None), r.headers
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                parsed = json.loads(raw) if raw else None
            except ValueError:
                parsed = raw.decode(errors="replace")
            return e.code, parsed, e.headers

    # -- JSON-RPC ---------------------------------------------------------
    def rpc_raw(self, method: str, params: Any = None) -> tuple[int, Any, Any]:
        self._id += 1
        payload = {"jsonrpc": "2.0", "id": self._id, "method": method, "params": [] if params is None else params}
        return self.http("POST", "/", payload)

    def rpc(self, method: str, params: Any = None) -> Any:
        """Return ``result`` or raise JsonRpcError / HttpStatusError."""
        status, out, headers = self.rpc_raw(method, params)
        if status != 200:
            raise HttpStatusError(status, out, headers)
        if "error" in out:
            e = out["error"]
            raise JsonRpcError(e["code"], e.get("message", ""), e.get("data"))
        return out["result"]

    # -- control API --------------------------------------------------------
    def ctl(self, method: str, path: str, json_body: Any = None) -> Any:
        """Call ``/_mock<path>``; raises AssertionError with the body on HTTP >= 300."""
        status, out, _ = self.http(method, "/_mock" + path, json_body)
        assert status < 300, f"{method} /_mock{path} -> {status}: {out}"
        return out

    def ctl_status(self, method: str, path: str, json_body: Any = None) -> tuple[int, Any]:
        status, out, _ = self.http(method, "/_mock" + path, json_body)
        return status, out

    # -- conveniences -------------------------------------------------------
    def mine(self, n: int = 1) -> None:
        self.ctl("POST", "/mine", {"n": n})

    def head(self) -> int:
        return int(self.rpc("eth_blockNumber"), 16)

    def create(self, **body: Any) -> str:
        """Create one entity in the pending block and return its key."""
        return self.ctl("POST", "/entities", body)["key"]

    def query(self, q: str, **opts: Any) -> dict[str, Any]:
        return self.rpc("arkiv_query", [q, opts] if opts else [q])

    def keys(self, q: str, **opts: Any) -> list[str]:
        return [e["key"] for e in self.query(q, **opts)["data"]]

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=10)


def _start_mock(*extra: str, seed: int = 7) -> MockArkiv:
    proc = subprocess.Popen(
        [sys.executable, str(MOCK_DIR / "server.py"), "--port", "0", "--seed", str(seed), *extra],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    assert proc.stdout is not None
    line = proc.stdout.readline().strip()
    if not line.startswith("READY "):
        proc.kill()
        raise RuntimeError(f"mock-arkiv did not print READY <port>, got {line!r}")
    return MockArkiv(proc, int(line.split()[1]))


@pytest.fixture
def mock_arkiv_factory() -> Iterator[Callable[..., MockArkiv]]:
    started: list[MockArkiv] = []

    def factory(*extra: str, seed: int = 7) -> MockArkiv:
        m = _start_mock(*extra, seed=seed)
        started.append(m)
        return m

    yield factory
    for m in started:
        m.stop()


@pytest.fixture
def mock_arkiv(mock_arkiv_factory: Callable[..., MockArkiv]) -> MockArkiv:
    return mock_arkiv_factory()


# ---------------------------------------------------------------------------
# PLACEHOLDER: `daemon` fixture (phase D2+). NOT IMPLEMENTED YET.
#
# Intended shape: start daemon/build/dev/arkivectord with a temp data dir pointed at `mock_arkiv`
# (RPC url = mock_arkiv.url), wait until it listens, yield an object with `.url` and `.get/.post`
# helpers, terminate at teardown. Until the binary exists every test using it is skipped.
# ---------------------------------------------------------------------------
@pytest.fixture
def daemon(mock_arkiv: MockArkiv) -> Iterator[Any]:
    if not DAEMON_BIN.exists():
        pytest.skip(f"daemon binary missing: {DAEMON_BIN}")
    pytest.skip("daemon fixture not implemented yet (placeholder for phase D2)")
    yield None  # pragma: no cover
