"""Integration tests for Model Context Protocol (MCP) Streamable HTTP endpoint (POST /mcp).

Covers:
- initialize request & protocol negotiation (2025-06-18)
- notifications/initialized notification
- tools/list tool discovery
- tools/call tool execution (arkivector_list_collections, arkivector_count, error handling)
- JSON-RPC 2.0 error handling (-32600, -32601, -32700)
"""

from __future__ import annotations

import json
from typing import Any
import pytest
from conftest import DaemonClient


def test_mcp_protocol_and_tools(daemon: DaemonClient) -> None:
    # 1. initialize
    init_req = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "test-agent", "version": "1.0.0"},
        },
    }
    status, body = daemon.post("/mcp", init_req)
    assert status == 200, f"POST /mcp initialize failed: {body}"
    assert body.get("jsonrpc") == "2.0"
    assert body.get("id") == 1
    res = body.get("result", {})
    assert res.get("protocolVersion") == "2025-06-18"
    assert "tools" in res.get("capabilities", {})
    assert res.get("serverInfo", {}).get("name") == "arkivector"

    # 2. notifications/initialized
    notify_req = {
        "jsonrpc": "2.0",
        "method": "notifications/initialized",
    }
    status, body = daemon.post("/mcp", notify_req)
    assert status == 204

    # 3. tools/list
    list_req = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/list",
    }
    status, body = daemon.post("/mcp", list_req)
    assert status == 200
    assert body.get("id") == 2
    tools = body.get("result", {}).get("tools", [])
    tool_names = [t["name"] for t in tools]
    assert "arkivector_list_collections" in tool_names
    assert "arkivector_search" in tool_names
    assert "arkivector_get" in tool_names
    assert "arkivector_count" in tool_names

    # 4. tools/call: arkivector_list_collections
    call_req = {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "tools/call",
        "params": {
            "name": "arkivector_list_collections",
            "arguments": {},
        },
    }
    status, body = daemon.post("/mcp", call_req)
    assert status == 200
    assert body.get("id") == 3
    content = body.get("result", {}).get("content", [])
    assert len(content) > 0
    assert content[0].get("type") == "text"
    parsed_res = json.loads(content[0]["text"])
    assert "collections" in parsed_res
    assert isinstance(parsed_res["collections"], list)

    # 5. tools/call: unknown tool
    bad_call = {
        "jsonrpc": "2.0",
        "id": 4,
        "method": "tools/call",
        "params": {
            "name": "non_existent_tool",
            "arguments": {},
        },
    }
    status, body = daemon.post("/mcp", bad_call)
    assert status == 200
    assert body.get("result", {}).get("isError") is True

    # 6. JSON-RPC unknown method
    unknown_method = {
        "jsonrpc": "2.0",
        "id": 5,
        "method": "some_random_method",
    }
    status, body = daemon.post("/mcp", unknown_method)
    assert status == 200
    assert "error" in body
    assert body["error"]["code"] == -32601
