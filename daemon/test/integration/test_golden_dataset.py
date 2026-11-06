"""Integration tests for the demo golden dataset.

Verifies all 4 queries in spec/fixtures/demo-dataset.json:
- returns the exact expected_ids in ascending distance order
- distances match expected_distances within distance_tolerance
- exact count matching filter equals expected_count_matching_filter
"""

from __future__ import annotations

import json
import math
import sys
import time
from pathlib import Path
from typing import Any

import pytest
from conftest import DaemonClient, MockArkiv

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
DEMO_PATH = REPO_ROOT / "spec" / "fixtures" / "demo-dataset.json"

import importlib.util

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
def seeded_collection(daemon: DaemonClient, mock_arkiv: MockArkiv, demo_fixture_data: dict[str, Any]) -> str:
    # 1. Seed demo entities into mock-arkiv
    seed(mock_arkiv.url, DEMO_PATH)

    # 2. Create the collection via POST /v1/collections
    col_def = demo_fixture_data["collection"]
    col_name = col_def["name"]
    status, body = daemon.post("/v1/collections", col_def)
    assert status in (200, 201), f"POST /v1/collections failed ({status}): {body}"

    # 3. Wait for collection to become live
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


def test_golden_dataset_all_queries(daemon: DaemonClient, seeded_collection: str, demo_fixture_data: dict[str, Any]) -> None:
    queries = demo_fixture_data["queries"]
    col_name = seeded_collection

    for q in queries:
        q_name = q["name"]
        req = q["request"]
        expected_ids = q["expected_ids"]
        expected_distances = q["expected_distances"]
        tolerance = q.get("distance_tolerance", 0.002)
        expected_count = q["expected_count_matching_filter"]

        # 1. Test search endpoint
        status, resp = daemon.post(f"/v1/collections/{col_name}/search", req)
        assert status == 200, f"Query '{q_name}' search returned status {status}: {resp}"

        results = resp.get("results", [])
        actual_ids = [r["id"] for r in results]
        assert actual_ids == expected_ids, (
            f"Query '{q_name}' ID mismatch:\nExpected: {expected_ids}\nActual:   {actual_ids}"
        )

        actual_distances = [r["distance"] for r in results]
        assert len(actual_distances) == len(expected_distances), (
            f"Query '{q_name}' result count mismatch: expected {len(expected_distances)}, got {len(actual_distances)}"
        )

        for i, (act, exp) in enumerate(zip(actual_distances, expected_distances)):
            assert math.isclose(act, exp, abs_tol=tolerance), (
                f"Query '{q_name}' hit {i} distance mismatch: expected {exp} ± {tolerance}, got {act}"
            )

        # 2. Test count endpoint with the same filter / where
        count_req: dict[str, Any] = {}
        if "filter" in req:
            count_req["filter"] = req["filter"]
        if "where" in req:
            count_req["where"] = req["where"]

        status, count_resp = daemon.post(f"/v1/collections/{col_name}/count", count_req)
        assert status == 200, f"Query '{q_name}' count returned status {status}: {count_resp}"
        actual_count = count_resp.get("count")
        assert actual_count == expected_count, (
            f"Query '{q_name}' count mismatch: expected {expected_count}, got {actual_count}"
        )
