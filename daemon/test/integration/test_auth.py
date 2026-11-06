"""Integration tests for API keys auth (auth.c) and rate limiting (ratelimit.c).

Covers:
- Unauthenticated endpoints (/healthz, /readyz, /v1/version, /metrics) pass without token
- Protected endpoints (/v1/collections, etc.) reject missing or invalid token with 401
- Token with read-only scope cannot perform admin actions (403 forbidden)
- Token with admin scope succeeds
- Rate limiter returns 429 when burst is exceeded
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Iterator
import pytest
from conftest import DAEMON_BIN, DaemonClient, MockArkiv, _free_port


@pytest.fixture
def auth_daemon(mock_arkiv: MockArkiv) -> Iterator[tuple[DaemonClient, str, str]]:
    """Daemon running with auth enabled (read_key and admin_key)."""
    port = _free_port()
    temp_dir = Path(tempfile.mkdtemp(prefix="arkivectord-auth-test-"))
    data_dir = temp_dir / "data"
    data_dir.mkdir(parents=True, exist_ok=True)

    read_token = "secret-read-token-123"
    admin_token = "secret-admin-token-456"

    read_sha = hashlib.sha256(read_token.encode("utf-8")).hexdigest()
    admin_sha = hashlib.sha256(admin_token.encode("utf-8")).hexdigest()

    keys_file = temp_dir / "api_keys.toml"
    keys_content = f"""
[[keys]]
id = "reader"
sha256 = "{read_sha}"
scopes = ["read"]

[[keys]]
id = "admin"
sha256 = "{admin_sha}"
scopes = ["read", "write", "admin"]
"""
    keys_file.write_text(keys_content)

    config_path = temp_dir / "config.toml"
    config_content = f"""
[server]
listen = "127.0.0.1:{port}"
api_keys_file = "{keys_file}"
ratelimit_rps = 5.0
ratelimit_burst = 5.0

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
    client = DaemonClient(proc, port, temp_dir)

    deadline = time.time() + 10.0
    while time.time() < deadline:
        if proc.poll() is not None:
            stdout, stderr = proc.communicate()
            raise RuntimeError(f"Daemon died on startup: code={proc.returncode}\nstdout={stdout}\nstderr={stderr}")
        try:
            status, body = client.get("/healthz")
            if status == 200 and body.get("status") == "ok":
                break
        except Exception:
            time.sleep(0.05)
    else:
        client.stop()
        raise TimeoutError("Timed out waiting for daemon with auth to start")

    yield client, read_token, admin_token
    client.stop()


def test_auth_and_scopes(auth_daemon: tuple[DaemonClient, str, str]) -> None:
    client, read_token, admin_token = auth_daemon

    # 1. Unauthenticated endpoints work without header
    st, body = client.get("/healthz")
    assert st == 200
    st, body = client.get("/readyz")
    assert st == 200
    st, body = client.get("/v1/version")
    assert st == 200
    st, body = client.get("/metrics")
    assert st == 200

    # 2. Protected endpoint /v1/collections without token -> 401
    st, body = client.get("/v1/collections")
    assert st == 401

    # 3. Protected endpoint with bad token -> 401
    st, body = client.get("/v1/collections", headers={"Authorization": "Bearer bad-token"})
    assert st == 401

    # 4. Protected endpoint with read token -> 200
    st, body = client.get("/v1/collections", headers={"Authorization": f"Bearer {read_token}"})
    assert st == 200

    # 5. Admin endpoint (POST /v1/collections) with read-only token -> 403
    col_def = {
        "name": "auth_test_col",
        "column": {"type": "halfvec", "dimensions": 4},
        "index": {"method": "none", "opclass": "halfvec_cosine_ops"},
        "writers": ["0x1111111111111111111111111111111111111111"],
    }
    st, body = client.post("/v1/collections", col_def, headers={"Authorization": f"Bearer {read_token}"})
    assert st == 403

    # 6. Admin endpoint with admin token -> 201
    st, body = client.post("/v1/collections", col_def, headers={"Authorization": f"Bearer {admin_token}"})
    assert st in (200, 201)

    # 7. Entity point lookup with ?verify=true
    st, body = client.get("/v1/collections/auth_test_col/entities/nonexistent?verify=true",
                          headers={"Authorization": f"Bearer {read_token}"})
    assert st == 404
    assert body.get("code") == "entity_not_found"

    # 8. Rate limit burst exceeded -> 429
    # Rapid-fire 20 requests
    got_429 = False
    for _ in range(25):
        st, body, hdrs = client.http("GET", "/v1/collections", headers={"Authorization": f"Bearer {read_token}"})
        if st == 429:
            got_429 = True
            assert "Retry-After" in hdrs
            break
    assert got_429, "Expected rate limit 429 after bursting requests"
