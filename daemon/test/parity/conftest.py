"""Fixtures for the pgvector parity suite.

Environment:
  PARITY_PG_DSN   libpq DSN (default postgresql://postgres:$PARITY_PG_PASSWORD@127.0.0.1:54329/parity)
  AV_PARITY_CLI   path to the av_parity_cli binary (default daemon/build/dev/test/av_parity_cli)
  PARITY_N        pairs per (type, operator) in test_distances.py (default 10000)
  PARITY_SEED     RNG seed (default 20260611)
Everything skips cleanly when the database or the binary is not available.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
DAEMON = HERE.parent.parent

_SUMMARY: list[str] = []


def _dsn() -> str:
    pw = os.environ.get("PARITY_PG_PASSWORD", "parity")
    return os.environ.get("PARITY_PG_DSN", f"postgresql://postgres:{pw}@127.0.0.1:54329/parity")


@pytest.fixture(scope="session")
def pg():
    try:
        import psycopg
    except ImportError:
        pytest.skip("psycopg is not installed (pip install -r daemon/test/parity/requirements.txt)")
    try:
        conn = psycopg.connect(_dsn(), autocommit=True, connect_timeout=3)
    except Exception as exc:  # noqa: BLE001
        pytest.skip(f"parity PostgreSQL not reachable ({exc}); run docker compose -f daemon/test/parity/docker-compose.yml up -d")
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    ver = conn.execute("SELECT extversion FROM pg_extension WHERE extname='vector'").fetchone()[0]
    _SUMMARY.append(f"PostgreSQL pgvector extension version: {ver}")
    yield conn
    conn.close()


class Cli:
    """Persistent av_parity_cli process speaking JSON lines."""

    def __init__(self, path: str) -> None:
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1
        )

    def run(self, reqs: list[dict]) -> list[dict]:
        """Send all requests, return one response per request (pipelined in chunks)."""
        out: list[dict] = []
        chunk = 200
        for i in range(0, len(reqs), chunk):
            part = reqs[i : i + chunk]
            self.proc.stdin.write("".join(json.dumps(r) + "\n" for r in part))
            self.proc.stdin.flush()
            for _ in part:
                line = self.proc.stdout.readline()
                if not line:
                    raise RuntimeError("av_parity_cli died (EOF)")
                out.append(json.loads(line))
        return out

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


@pytest.fixture(scope="session")
def cli():
    path = os.environ.get("AV_PARITY_CLI", str(DAEMON / "build" / "dev" / "test" / "av_parity_cli"))
    if not os.path.isfile(path) or not os.access(path, os.X_OK):
        pytest.skip(f"av_parity_cli not built at {path} (cmake --build --preset dev --target av_parity_cli)")
    c = Cli(path)
    yield c
    c.close()


@pytest.fixture(scope="session")
def summary():
    return _SUMMARY


def pytest_terminal_summary(terminalreporter):
    if _SUMMARY:
        terminalreporter.section("pgvector parity summary")
        for line in _SUMMARY:
            terminalreporter.write_line(line)
