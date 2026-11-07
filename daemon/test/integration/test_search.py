"""Integration tests for search API options and edge cases.

Tests:
- k and offset bounds (1..1000, offset + k <= 1000)
- vector vs vector_bytes identical results
- max_distance and min_score thresholds
- include projections (document, vector, attributes)
- min_block consistency wait and timeout
"""

from __future__ import annotations

import importlib.util
import json
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
    import time
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


def test_search_k_offset_bounds(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    vec = demo_fixture_data["queries"][0]["request"]["vector"]

    # k = 0 is invalid (min: 1)
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 0})
    assert status in (400, 422), f"Expected 400/422 for k=0, got {status}: {resp}"

    # k = 1001 is invalid (max: 1000)
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 1001})
    assert status in (400, 422), f"Expected 400/422 for k=1001, got {status}: {resp}"

    # offset = -1 is invalid (min: 0)
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 5, "offset": -1})
    assert status in (400, 422), f"Expected 400/422 for offset=-1, got {status}: {resp}"

    # offset = 1000 is invalid (max: 999)
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 5, "offset": 1000})
    assert status in (400, 422), f"Expected 400/422 for offset=1000, got {status}: {resp}"

    # offset + k > 1000 is invalid
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 600, "offset": 500})
    assert status in (400, 422), f"Expected 400/422 for offset+k > 1000, got {status}: {resp}"

    # Valid offset pagination: k=3, offset=2 matches results[2:5] of k=5, offset=0
    status, resp_full = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 5, "offset": 0})
    assert status == 200
    status, resp_page = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 3, "offset": 2})
    assert status == 200

    full_ids = [r["id"] for r in resp_full["results"]]
    page_ids = [r["id"] for r in resp_page["results"]]
    assert page_ids == full_ids[2:5]


def test_search_vector_vs_vector_bytes(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    first_entity = demo_fixture_data["entities"][0]
    vec = first_entity["vector"]
    vec_bytes = first_entity["vector_bytes"]

    # Search using vector
    status_v, resp_v = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 5, "exact": True},
    )
    assert status_v == 200, f"vector search failed: {resp_v}"

    # Search using vector_bytes
    status_b, resp_b = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector_bytes": vec_bytes, "k": 5, "exact": True},
    )
    assert status_b == 200, f"vector_bytes search failed: {resp_b}"

    # Verify identical results
    ids_v = [r["id"] for r in resp_v["results"]]
    ids_b = [r["id"] for r in resp_b["results"]]
    assert ids_v == ids_b

    dists_v = [r["distance"] for r in resp_v["results"]]
    dists_b = [r["distance"] for r in resp_b["results"]]
    for dv, db in zip(dists_v, dists_b):
        assert abs(dv - db) < 1e-5


def test_search_distance_and_score_thresholds(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    vec = demo_fixture_data["queries"][0]["request"]["vector"]

    # Baseline: k=10
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 10})
    assert status == 200
    results = resp.get("results", [])
    assert len(results) >= 5

    # Threshold at 3rd hit distance
    target_dist = results[3]["distance"]
    status, resp_max_dist = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 10, "max_distance": target_dist},
    )
    assert status == 200
    for r in resp_max_dist.get("results", []):
        assert r["distance"] <= target_dist + 1e-6

    # min_score threshold (for cosine: score = 1.0 - distance)
    target_score = results[3]["score"]
    status, resp_min_score = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 10, "min_score": target_score},
    )
    assert status == 200
    for r in resp_min_score.get("results", []):
        assert r["score"] >= target_score - 1e-6


def test_search_include_projections(daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]) -> None:
    vec = demo_fixture_data["queries"][0]["request"]["vector"]

    # Default include: ["attributes"]
    status, resp = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 2, "include": ["attributes"]},
    )
    assert status == 200
    r0 = resp["results"][0]
    assert r0.get("attributes") is not None
    assert r0.get("document") is None
    assert r0.get("vector") is None

    # Include document only
    status, resp_doc = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 2, "include": ["document"]},
    )
    assert status == 200
    r0_doc = resp_doc["results"][0]
    assert isinstance(r0_doc.get("document"), str)
    assert len(r0_doc["document"]) > 0
    assert r0_doc.get("vector") is None

    # Include vector only (populates vector_bytes)
    status, resp_vec = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 2, "include": ["vector"]},
    )
    assert status == 200
    r0_vec = resp_vec["results"][0]
    assert isinstance(r0_vec.get("vector_bytes"), str)
    assert len(r0_vec["vector_bytes"]) > 0
    assert r0_vec.get("document") is None

    # Include all: document, vector, attributes
    status, resp_all = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 2, "include": ["document", "vector", "attributes"]},
    )
    assert status == 200
    r0_all = resp_all["results"][0]
    assert r0_all.get("attributes") is not None
    assert isinstance(r0_all.get("document"), str)
    assert isinstance(r0_all.get("vector_bytes"), str)


def test_search_consistency_wait_and_timeout(
    daemon: DaemonClient, mock_arkiv: MockArkiv, collection_name: str, demo_fixture_data: dict[str, Any]
) -> None:
    vec = demo_fixture_data["queries"][0]["request"]["vector"]
    head = mock_arkiv.head()

    # min_block <= head succeeds immediately
    status, resp = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 2, "consistency": {"min_block": head, "timeout_ms": 2000}},
    )
    assert status == 200, f"Expected 200 with min_block <= head, got {status}: {resp}"
    assert resp["as_of_block"] >= head

    # min_block far in future exceeds timeout_ms -> 504 consistency_timeout
    status, resp_timeout = daemon.post(
        f"/v1/collections/{collection_name}/search",
        {"vector": vec, "k": 2, "consistency": {"min_block": head + 100000, "timeout_ms": 200}},
    )
    assert status == 504, f"Expected 504 consistency timeout, got {status}: {resp_timeout}"
    assert resp_timeout.get("code") == "consistency_timeout"


def test_search_attributes_survive_response_serialization(
    daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]
) -> None:
    """Regression: attribute names in the response pointed into a freed arena (heap use-after-free).

    Under the release build this crashed the daemon on the first search that returned attributes.
    Many searches in a row make a stale pointer far more likely to be overwritten."""
    vec = demo_fixture_data["queries"][0]["request"]["vector"]
    for _ in range(40):
        status, resp = daemon.post(
            f"/v1/collections/{collection_name}/search",
            {"vector": vec, "k": 20, "include": ["attributes", "document"]},
        )
        assert status == 200
        names = {name for hit in resp["results"] for name in hit["attributes"]}
        assert names, "hits should carry attributes"
        assert all(n.isidentifier() or n.replace(".", "").replace("_", "").isalnum() for n in names), names


def test_get_entity_decodes_percent_encoded_include(
    daemon: DaemonClient, collection_name: str, demo_fixture_data: dict[str, Any]
) -> None:
    """Regression: include=attributes%2Cdocument (what the SDKs send) used to be ignored."""
    vec = demo_fixture_data["queries"][0]["request"]["vector"]
    status, resp = daemon.post(f"/v1/collections/{collection_name}/search", {"vector": vec, "k": 1})
    assert status == 200
    ref = resp["results"][0]["id"]
    status, plain = daemon.get(f"/v1/collections/{collection_name}/entities/{ref}?include=attributes,document")
    status2, encoded = daemon.get(f"/v1/collections/{collection_name}/entities/{ref}?include=attributes%2Cdocument")
    assert status == status2 == 200
    assert isinstance(plain["entity"].get("document"), str)
    assert encoded["entity"].get("document") == plain["entity"]["document"]
    assert encoded["entity"].get("attributes") == plain["entity"]["attributes"]
