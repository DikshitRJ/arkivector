"""Integration tests for write endpoints and search:batch.

Covers:
- POST /v1/collections/{name}/entities:prepare (unsigned tx calldata, predicted key, op="create")
- Idempotency-Key header on write endpoints (caching, repeated identical response, conflict rejection)
- POST /v1/collections/{name}/search:batch (multi-query atomic execution against single snapshot)
"""

from __future__ import annotations

import importlib.util
import json
import time
import uuid
from pathlib import Path
from typing import Any

import pytest
from conftest import DaemonClient, MockArkiv

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
DEMO_PATH = REPO_ROOT / "spec" / "fixtures" / "demo-dataset.json"

TOOLS_DIR = REPO_ROOT / "daemon" / "tools"
_seed_demo_spec = importlib.util.spec_from_file_location("seed_demo", TOOLS_DIR / "seed-demo.py")
_seed_demo = importlib.util.module_from_spec(_seed_demo_spec)
assert _seed_demo_spec.loader is not None
_seed_demo_spec.loader.exec_module(_seed_demo)
seed = _seed_demo.seed


@pytest.fixture(scope="module")
def demo_fixture_data() -> dict[str, Any]:
    with open(DEMO_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


@pytest.fixture
def collection_name(daemon: DaemonClient, mock_arkiv: MockArkiv, demo_fixture_data: dict[str, Any]) -> str:
    seed(mock_arkiv.url, DEMO_PATH)
    col_def = demo_fixture_data["collection"]
    col_name = col_def["name"]
    status, body = daemon.post("/v1/collections", col_def)
    assert status in (200, 201), f"POST /v1/collections failed ({status}): {body}"

    # Wait for collection to reach live state
    deadline = time.time() + 15.0
    while time.time() < deadline:
        status, body = daemon.get("/readyz")
        if status == 200 and body.get("ready"):
            cols = body.get("collections", {})
            if cols.get(col_name) == "live":
                break
        time.sleep(0.1)
    else:
        status, body = daemon.get("/readyz")
        raise TimeoutError(f"Collection {col_name} did not become live. readyz: {body}")

    return col_name


def test_entities_prepare(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    owner = "0x1111111111111111111111111111111111111111"
    first_vec = demo_fixture_data["entities"][0]["vector"]

    prepare_req = {
        "from": owner,
        "entities": [
            {
                "id": "doc-prepare-1",
                "ttl": {"days": 1},  # demo.docs has no default_ttl (collections.md)
                "vector": first_vec,
                "attributes": {
                    "title": {"type": "str", "value": "Test Document Prepare"}
                }
            }
        ]
    }

    status, body = daemon.post(f"/v1/collections/{collection_name}/entities:prepare", prepare_req)
    assert status == 200, f"POST entities:prepare failed ({status}): {body}"

    # Verify transactions array
    assert "transactions" in body, f"Expected 'transactions' array in response: {body}"
    txs = body["transactions"]
    assert isinstance(txs, list) and len(txs) >= 1
    tx = txs[0]
    assert "to" in tx
    assert tx["to"].lower() == "0x4400000000000000000000000000000000000044"
    assert "data" in tx
    assert tx["data"].startswith("0x49650044")
    assert "nonce" in tx
    assert isinstance(tx["nonce"], int)
    assert "gas_estimate" in tx
    assert isinstance(tx["gas_estimate"], int)
    assert tx["gas_estimate"] > 0

    # Verify results array
    assert "results" in body, f"Expected 'results' array in response: {body}"
    results = body["results"]
    assert isinstance(results, list) and len(results) == 1
    res = results[0]
    assert res.get("id") == "doc-prepare-1"
    assert "key" in res
    assert res["key"].startswith("0x") and len(res["key"]) == 66
    assert res.get("op") == "create"


def test_write_idempotency_key(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    first_vec = demo_fixture_data["entities"][0]["vector"]
    idem_key = str(uuid.uuid4())
    headers = {"Idempotency-Key": idem_key}

    req_body = {
        "from": "0x2222222222222222222222222222222222222222",
        "entities": [
            {
                "id": "doc-idem-1",
                "ttl": {"days": 1},  # demo.docs has no default_ttl (collections.md)
                "vector": first_vec,
                "attributes": {
                    "tag": {"type": "str", "value": "idempotent-write"}
                }
            }
        ]
    }

    # Initial request
    status1, body1 = daemon.post(
        f"/v1/collections/{collection_name}/entities:prepare", req_body, headers=headers
    )
    assert status1 == 200, f"First request failed ({status1}): {body1}"

    # Repeated request with same Idempotency-Key and identical payload
    status2, body2 = daemon.post(
        f"/v1/collections/{collection_name}/entities:prepare", req_body, headers=headers
    )
    assert status2 == status1
    assert body2 == body1, f"Responses differ for same Idempotency-Key:\n{body1}\nvs\n{body2}"

    # Same Idempotency-Key with differing payload triggers 409 conflict
    conflicting_body = {
        "from": "0x2222222222222222222222222222222222222222",
        "entities": [
            {
                "id": "doc-idem-2-conflicting",
                "ttl": {"days": 1},  # demo.docs has no default_ttl (collections.md)
                "vector": first_vec,
                "attributes": {
                    "tag": {"type": "str", "value": "different-write"}
                }
            }
        ]
    }
    status_conflict, body_conflict = daemon.post(
        f"/v1/collections/{collection_name}/entities:prepare", conflicting_body, headers=headers
    )
    assert status_conflict == 409, f"Expected 409 for conflicting Idempotency-Key, got {status_conflict}: {body_conflict}"


def test_search_batch(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    queries = demo_fixture_data["queries"]
    vec1 = queries[0]["request"]["vector"]
    vec2 = queries[1]["request"]["vector"]

    batch_req = {
        "searches": [
            {"vector": vec1, "k": 3},
            {"vector": vec2, "k": 4}
        ]
    }

    status, body = daemon.post(f"/v1/collections/{collection_name}/search:batch", batch_req)
    assert status == 200, f"search:batch failed ({status}): {body}"
    assert "as_of_block" in body
    assert "responses" in body
    assert isinstance(body["responses"], list)
    assert len(body["responses"]) == 2

    resp1 = body["responses"][0]
    resp2 = body["responses"][1]
    assert "results" in resp1
    assert "results" in resp2
    assert len(resp1["results"]) <= 3
    assert len(resp2["results"]) <= 4

    # Consistency with single search endpoint
    status_single1, body_single1 = daemon.post(
        f"/v1/collections/{collection_name}/search", {"vector": vec1, "k": 3}
    )
    assert status_single1 == 200
    assert [r["id"] for r in resp1["results"]] == [r["id"] for r in body_single1["results"]]

    status_single2, body_single2 = daemon.post(
        f"/v1/collections/{collection_name}/search", {"vector": vec2, "k": 4}
    )
    assert status_single2 == 200
    assert [r["id"] for r in resp2["results"]] == [r["id"] for r in body_single2["results"]]
