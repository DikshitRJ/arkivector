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


import shutil
import socket
import tempfile
import time


class DaemonClient:
    """Wrapper around a running arkivectord instance."""

    def __init__(self, proc: subprocess.Popen[str], port: int, data_dir: Path) -> None:
        self.proc = proc
        self.port = port
        self.url = f"http://127.0.0.1:{port}"
        self.data_dir = data_dir

    def http(
        self, method: str, path: str, body: Any = None, headers: dict[str, str] | None = None
    ) -> tuple[int, Any, Any]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        req_headers = {"Content-Type": "application/json"}
        if headers:
            req_headers.update(headers)
        req = urllib.request.Request(self.url + path, data=data, method=method, headers=req_headers)
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

    def get(self, path: str, headers: dict[str, str] | None = None) -> tuple[int, Any]:
        status, body, _ = self.http("GET", path, headers=headers)
        return status, body

    def post(self, path: str, body: Any = None, headers: dict[str, str] | None = None) -> tuple[int, Any]:
        status, body, _ = self.http("POST", path, body, headers=headers)
        return status, body

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


@pytest.fixture
def daemon(mock_arkiv: MockArkiv) -> Iterator[DaemonClient]:
    if not DAEMON_BIN.exists():
        pytest.skip(f"daemon binary missing: {DAEMON_BIN}")

    port = _free_port()
    temp_dir = Path(tempfile.mkdtemp(prefix="arkivectord-test-"))
    data_dir = temp_dir / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    config_path = temp_dir / "config.toml"

    config_content = f"""
[server]
listen = "127.0.0.1:{port}"

[arkiv]
rpc_urls = ["{mock_arkiv.url}"]
ws_url = "{mock_arkiv.ws_url}"
chain_id = 7794095
confirmations = 0

[storage]
data_dir = "{data_dir}"
map_size = 104857600

[sync]
poll_interval_ms = 50

[log]
level = "debug"
format = "text"
"""
    config_path.write_text(config_content)

    proc = subprocess.Popen(
        [str(DAEMON_BIN), "--config", str(config_path), "--debug-routes"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    client = DaemonClient(proc, port, data_dir)

    # Poll /readyz until ready or timeout
    deadline = time.time() + 10.0
    ready = False
    while time.time() < deadline:
        if proc.poll() is not None:
            stdout, stderr = proc.communicate()
            raise RuntimeError(
                f"arkivectord exited unexpectedly with code {proc.returncode}:\nstdout: {stdout}\nstderr: {stderr}"
            )
        try:
            status, body = client.get("/readyz")
            if status == 200:
                ready = True
                break
        except Exception:
            pass
        time.sleep(0.05)

    if not ready:
        client.stop()
        raise RuntimeError("Timed out waiting for arkivectord /readyz")

    try:
        yield client
    finally:
        client.stop()
        shutil.rmtree(temp_dir, ignore_errors=True)
