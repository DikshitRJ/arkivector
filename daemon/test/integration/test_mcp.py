"""Integration tests for the MCP server (POST /mcp, Streamable HTTP, protocol 2025-06-18).

Covers:
- initialize (version negotiation, instructions), ping, notifications (202), tools/list
- transport rules: GET /mcp -> 405, JSON-RPC batches rejected, parse errors, unknown method / tool
- Origin validation (DNS rebinding): non-loopback origins rejected unless API-key auth is on
- tools end to end with a signer: upsert, list_collections, search (where / filter / documents),
  count, get, and errors returned as isError results carrying the /v1 problem object
- API-key scopes apply to each tool's inner /v1 call: a read key can search but not upsert
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any, Iterator, Optional

import pytest
from conftest import DAEMON_BIN, DaemonClient, MockArkiv, _free_port

pytestmark = pytest.mark.daemon

DEV_KEY = "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"  # public test key
DEV_ADDR = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"


def rpc(
    daemon: DaemonClient,
    method: str,
    params: Optional[dict[str, Any]] = None,
    rid: Optional[int] = 1,
    token: Optional[str] = None,
    origin: Optional[str] = None,
) -> tuple[int, Any]:
    msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
    if rid is not None:
        msg["id"] = rid
    if params is not None:
        msg["params"] = params
    headers = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if origin:
        headers["Origin"] = origin
    status, body, _ = daemon.http("POST", "/mcp", msg, headers=headers)
    return status, body


def call(daemon: DaemonClient, tool: str, args: dict[str, Any], token: Optional[str] = None) -> dict[str, Any]:
    status, body = rpc(daemon, "tools/call", {"name": tool, "arguments": args}, token=token)
    assert status == 200, body
    assert "result" in body, body
    return body["result"]


# --------------------------------------------------------------------------- protocol


def test_mcp_protocol(daemon: DaemonClient) -> None:
    status, body = rpc(
        daemon,
        "initialize",
        {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}},
    )
    assert status == 200, body
    res = body["result"]
    assert res["protocolVersion"] == "2025-06-18"
    assert "tools" in res["capabilities"]
    assert res["serverInfo"]["name"] == "arkivector"
    assert "arkivector_list_collections" in res["instructions"]

    # older supported version is echoed; unknown versions get the latest
    for asked, got in (("2025-03-26", "2025-03-26"), ("2099-01-01", "2025-06-18")):
        _, body = rpc(daemon, "initialize", {"protocolVersion": asked, "capabilities": {}})
        assert body["result"]["protocolVersion"] == got

    # notifications are accepted with 202 and no body (MCP Streamable HTTP)
    status, body = rpc(daemon, "notifications/initialized", rid=None)
    assert status == 202

    status, body = rpc(daemon, "ping", rid=7)
    assert status == 200 and body == {"jsonrpc": "2.0", "id": 7, "result": {}}

    _, body = rpc(daemon, "tools/list", rid=2)
    tools = {t["name"]: t for t in body["result"]["tools"]}
    assert set(tools) >= {"arkivector_list_collections", "arkivector_search", "arkivector_get", "arkivector_count"}
    assert "arkivector_upsert" not in tools  # no signer on this daemon
    assert tools["arkivector_search"]["annotations"]["readOnlyHint"] is True
    props = tools["arkivector_search"]["inputSchema"]["properties"]
    assert set(props) >= {"collection", "vector", "k", "where", "filter", "include_document", "text"}

    _, body = rpc(daemon, "some_random_method", rid=5)
    assert body["error"]["code"] == -32601
    _, body = rpc(daemon, "tools/call", {"name": "no_such_tool", "arguments": {}}, rid=6)
    assert body["error"]["code"] == -32602

    # no server-initiated stream: GET is 405 with Allow: POST
    status, _, headers = daemon.http("GET", "/mcp")
    assert status == 405
    assert "POST" in (headers.get("Allow") or "")

    # JSON-RPC batches were removed in 2025-06-18
    status, body, _ = daemon.http("POST", "/mcp", [{"jsonrpc": "2.0", "id": 1, "method": "ping"}])
    assert status == 400 and body["error"]["code"] == -32600


def test_mcp_origin_validation(daemon: DaemonClient) -> None:
    status, body = rpc(daemon, "ping", origin="https://evil.example")
    assert status == 403, body
    assert body["code"] == "forbidden"
    for origin in ("http://localhost:6274", "http://127.0.0.1", "http://[::1]:3000"):
        status, body = rpc(daemon, "ping", origin=origin)
        assert status == 200, (origin, body)


# --------------------------------------------------------------------------- tools, auth


@pytest.fixture
def mcp_daemon(mock_arkiv: MockArkiv) -> Iterator[tuple[DaemonClient, dict[str, str]]]:
    """Daemon with a dev signer and API keys (read, write, admin)."""
    if not DAEMON_BIN.exists():
        if os.environ.get("AV_REQUIRE_DAEMON") == "1":
            pytest.fail(f"daemon binary missing: {DAEMON_BIN}")
        pytest.skip(f"daemon binary missing: {DAEMON_BIN}")
    tokens = {"read": "mcp-read-token", "write": "mcp-write-token", "admin": "mcp-admin-token"}
    scopes = {"read": '["read"]', "write": '["read", "write"]', "admin": '["read", "write", "admin"]'}
    port = _free_port()
    tmp = Path(tempfile.mkdtemp(prefix="arkivectord-mcp-"))
    (tmp / "data").mkdir()
    keys = "".join(
        f'[[keys]]\nid = "{k}"\nsha256 = "{hashlib.sha256(v.encode()).hexdigest()}"\nscopes = {scopes[k]}\n\n'
        for k, v in tokens.items()
    )
    (tmp / "keys.toml").write_text(keys)
    (tmp / "config.toml").write_text(
        f"""
[server]
listen = "127.0.0.1:{port}"
api_keys_file = "{tmp / 'keys.toml'}"

[arkiv]
rpc_urls = ["{mock_arkiv.url}"]
ws_url = "{mock_arkiv.ws_url}"
chain_id = 7794095
confirmations = 0

[storage]
data_dir = "{tmp / 'data'}"
map_size = 104857600

[sync]
poll_interval_ms = 50
"""
    )
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 200})  # writes wait for their receipt
    env = {**os.environ, "ARKIV_PRIVATE_KEY": DEV_KEY}
    proc = subprocess.Popen(
        [str(DAEMON_BIN), "--config", str(tmp / "config.toml")],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
        env=env,
    )
    client = DaemonClient(proc, port, tmp / "data")
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            if client.get("/healthz")[0] == 200:
                break
        except Exception:
            pass
        time.sleep(0.05)
    try:
        yield client, tokens
    finally:
        client.stop()
        shutil.rmtree(tmp, ignore_errors=True)


def test_mcp_tools_end_to_end(mcp_daemon: tuple[DaemonClient, dict[str, str]]) -> None:
    daemon, tok = mcp_daemon
    admin = {"Authorization": f"Bearer {tok['admin']}"}

    # without a key, /mcp itself is refused
    status, _ = rpc(daemon, "ping")
    assert status == 401

    status, body, _ = daemon.http(
        "POST",
        "/v1/collections",
        {
            "name": "notes",
            "column": {"type": "halfvec", "dimensions": 4},
            "index": {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
            "writers": [DEV_ADDR],
            "attributes": {"topic": "str", "stars": "i32"},
            "default_ttl": {"days": 7},
        },
        headers=admin,
    )
    assert status == 201, body

    _, body = rpc(daemon, "tools/list", token=tok["write"])
    assert "arkivector_upsert" in {t["name"] for t in body["result"]["tools"]}

    entities = [
        {"id": "cat", "vector": [1, 0, 0, 0], "document": "Cats purr.", "attributes": {"topic": "pets", "stars": 5}},
        {"id": "dog", "vector": [0.9, 0.1, 0, 0], "document": "Dogs bark.", "attributes": {"topic": "pets", "stars": 4}},
        {"id": "car", "vector": [0, 0, 1, 0], "document": "Cars need fuel.", "attributes": {"topic": "vehicles", "stars": 3}},
    ]
    # a read key cannot write through MCP: the inner /v1 call needs the write scope
    res = call(daemon, "arkivector_upsert", {"collection": "notes", "entities": entities}, token=tok["read"])
    assert res["isError"] is True
    assert json.loads(res["content"][0]["text"])["code"] == "forbidden"

    res = call(daemon, "arkivector_upsert", {"collection": "notes", "entities": entities}, token=tok["write"])
    assert res["isError"] is False, res
    assert [r["op"] for r in res["structuredContent"]["results"]] == ["create", "create", "create"]

    res = call(daemon, "arkivector_list_collections", {}, token=tok["read"])
    assert res["isError"] is False
    assert "notes: halfvec(4)" in res["content"][0]["text"]
    assert [c["name"] for c in res["structuredContent"]["collections"]] == ["notes"]

    res = call(
        daemon,
        "arkivector_search",
        {"collection": "notes", "vector": [1, 0.05, 0, 0], "k": 2, "where": "topic = str('pets')", "include_document": True},
        token=tok["read"],
    )
    assert res["isError"] is False, res
    hits = res["structuredContent"]["results"]
    assert [h["id"] for h in hits] == ["cat", "dog"]
    assert hits[0]["document"] == "Cats purr."
    assert hits[0]["distance"] < hits[1]["distance"]
    assert json.loads(res["content"][0]["text"])["results"][0]["id"] == "cat"

    res = call(daemon, "arkivector_search", {"collection": "notes", "vector": [0, 0, 1, 0], "filter": {"gte": {"stars": 4}}}, token=tok["read"])
    assert {h["id"] for h in res["structuredContent"]["results"]} == {"cat", "dog"}

    res = call(daemon, "arkivector_count", {"collection": "notes", "filter": {"eq": {"topic": "pets"}}}, token=tok["read"])
    assert res["structuredContent"]["count"] == 2
    res = call(daemon, "arkivector_count", {"collection": "notes"}, token=tok["read"])
    assert res["structuredContent"]["count"] == 3

    res = call(daemon, "arkivector_get", {"collection": "notes", "ref": "cat", "include_document": True}, token=tok["read"])
    ent = res["structuredContent"]["entity"]
    assert ent["document"] == "Cats purr."
    assert ent["attributes"]["stars"] == {"type": "i32", "value": 5}

    # errors are tool results with the /v1 problem object as text
    for args, code in (
        ({"collection": "notes", "vector": [1, 0], "k": 2}, "dimension_mismatch"),
        ({"collection": "nope", "vector": [1, 0, 0, 0]}, "collection_not_found"),
        ({"collection": "notes", "vector": [1, 0, 0, 0], "where": "topic = 'pets'"}, "invalid_filter"),
    ):
        res = call(daemon, "arkivector_search", args, token=tok["read"])
        assert res["isError"] is True
        assert json.loads(res["content"][0]["text"])["code"] == code
    res = call(daemon, "arkivector_get", {"collection": "notes", "ref": "nope"}, token=tok["read"])
    assert res["isError"] is True and json.loads(res["content"][0]["text"])["code"] == "entity_not_found"
    res = call(daemon, "arkivector_search", {"collection": "notes", "text": "cats"}, token=tok["read"])
    assert res["isError"] is True and "embedder" in res["content"][0]["text"]
    res = call(daemon, "arkivector_search", {"collection": "notes", "vector": [1, 0, 0, 0], "k": 51}, token=tok["read"])
    assert res["isError"] is True

    # with auth on, a browser origin is allowed (the key is the protection)
    status, body = rpc(daemon, "ping", token=tok["read"], origin="https://app.example")
    assert status == 200, body
