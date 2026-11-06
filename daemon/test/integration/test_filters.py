"""Integration tests for filter evaluation.

Tests:
- WHERE vs JSON equivalence (both forms return identical results and counts)
- Random / diverse filters evaluated by daemon count vs mock-arkiv count
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
def collection_info(daemon: DaemonClient, mock_arkiv: MockArkiv, demo_fixture_data: dict[str, Any]) -> tuple[str, str]:
    seed(mock_arkiv.url, DEMO_PATH)
    col_def = demo_fixture_data["collection"]
    col_name = col_def["name"]
    schema_tag = col_def["schema_tag"]

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

    return col_name, schema_tag


EQUIVALENCE_CASES = [
    (
        "category = str('finance')",
        {"eq": {"category": "finance"}},
    ),
    (
        "score >= dec(4.0) AND published = true",
        {
            "and": [
                {"gte": {"score": {"type": "dec", "value": "4.0"}}},
                {"eq": {"published": True}},
            ]
        },
    ),
    (
        "rank BETWEEN -100 AND 100",
        {"between": {"rank": [{"type": "i32", "value": -100}, {"type": "i32", "value": 100}]}},
    ),
    (
        "category STARTSWITH str('sci')",
        {"startswith": {"category": "sci"}},
    ),
    (
        "EXISTS(rank)",
        {"exists": "rank"},
    ),
    (
        "TYPEOF(rank) = i32",
        {"typeof": {"rank": "i32"}},
    ),
    (
        "NOT (category = str('finance'))",
        {"not": {"eq": {"category": "finance"}}},
    ),
]


@pytest.mark.parametrize("where_clause,json_filter", EQUIVALENCE_CASES)
def test_filter_where_vs_json_equivalence(
    daemon: DaemonClient,
    collection_info: tuple[str, str],
    demo_fixture_data: dict[str, Any],
    where_clause: str,
    json_filter: dict[str, Any],
) -> None:
    col_name, _ = collection_info
    vec = demo_fixture_data["queries"][0]["request"]["vector"]

    # 1. Compare /count
    status_w, resp_w = daemon.post(f"/v1/collections/{col_name}/count", {"where": where_clause})
    assert status_w == 200, f"WHERE count failed for {where_clause}: {resp_w}"

    status_j, resp_j = daemon.post(f"/v1/collections/{col_name}/count", {"filter": json_filter})
    assert status_j == 200, f"JSON count failed for {json_filter}: {resp_j}"

    assert resp_w["count"] == resp_j["count"], (
        f"Count mismatch between WHERE ({resp_w['count']}) and JSON ({resp_j['count']})"
    )

    # 2. Compare /search
    status_sw, resp_sw = daemon.post(
        f"/v1/collections/{col_name}/search",
        {"vector": vec, "k": 5, "where": where_clause, "exact": True},
    )
    assert status_sw == 200

    status_sj, resp_sj = daemon.post(
        f"/v1/collections/{col_name}/search",
        {"vector": vec, "k": 5, "filter": json_filter, "exact": True},
    )
    assert status_sj == 200

    ids_w = [r["id"] for r in resp_sw["results"]]
    ids_j = [r["id"] for r in resp_sj["results"]]
    assert ids_w == ids_j


DIVERSE_FILTERS = [
    "category = str('finance')",
    "category = str('science')",
    "published = true",
    "published = false",
    "score >= dec(4.0)",
    "score < dec(3.0)",
    "rank > 0",
    "rank <= -20",
    "rank BETWEEN -50 AND 50",
    "category = str('tech') AND rank > 10",
    "category = str('science') OR published = true",
    "NOT (category = str('tech'))",
    "category STARTSWITH str('sci')",
]


@pytest.mark.parametrize("predicate", DIVERSE_FILTERS)
def test_daemon_count_vs_mock_arkiv(
    daemon: DaemonClient, mock_arkiv: MockArkiv, collection_info: tuple[str, str], predicate: str
) -> None:
    col_name, schema_tag = collection_info

    # Query mock-arkiv with count projection
    mock_query = f"arkiv.schema = str('{schema_tag}') AND ({predicate})"
    mock_res = mock_arkiv.query(mock_query, select={"count": True})
    expected_count = int(mock_res["count"], 16) if isinstance(mock_res["count"], str) else mock_res["count"]

    # Query daemon count
    status, daemon_res = daemon.post(f"/v1/collections/{col_name}/count", {"where": predicate})
    assert status == 200, f"Daemon count failed for predicate '{predicate}': {daemon_res}"
    actual_count = daemon_res.get("count")

    assert actual_count == expected_count, (
        f"Count mismatch for '{predicate}': daemon={actual_count}, mock-arkiv={expected_count}"
    )
