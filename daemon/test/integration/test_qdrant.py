"""Integration tests for Qdrant compatibility endpoints (/qdrant/*).

Covers:
- GET /qdrant/collections (list collections)
- PUT /qdrant/collections/{name} (create collection)
- GET /qdrant/collections/{name} (collection info & config)
- POST /qdrant/collections/{name}/points/count (count live points)
- POST /qdrant/collections/{name}/points/search (vector search with limit/offset)
- DELETE /qdrant/collections/{name} (delete collection)
"""

from __future__ import annotations

import time
from typing import Any
import pytest
from conftest import DaemonClient, MockArkiv


def test_qdrant_lifecycle_and_search(daemon: DaemonClient) -> None:
    # 1. List collections initially
    status, body = daemon.get("/qdrant/collections")
    assert status == 200, f"GET /qdrant/collections failed: {body}"
    assert body.get("status") == "ok"
    assert "result" in body
    assert "collections" in body["result"]
    assert "time" in body

    # 2. Create collection via Qdrant PUT
    col_name = "test_qdrant_col"
    create_body = {
        "vectors": {
            "size": 4,
            "distance": "Cosine",
        }
    }
    status, body, _ = daemon.http("PUT", f"/qdrant/collections/{col_name}", create_body)
    assert status == 200, f"PUT /qdrant/collections/{col_name} failed: {body}"
    assert body.get("status") == "ok"
    assert body.get("result") is True

    # 3. Get collection info
    status, body = daemon.get(f"/qdrant/collections/{col_name}")
    assert status == 200, f"GET /qdrant/collections/{col_name} failed: {body}"
    assert body.get("status") == "ok"
    res = body.get("result", {})
    assert res.get("status") == "green"
    cfg = res.get("config", {}).get("params", {}).get("vectors", {})
    assert cfg.get("size") == 4
    assert cfg.get("distance") == "Cosine"

    # 4. Count points (should be 0 initially)
    status, body = daemon.post(f"/qdrant/collections/{col_name}/points/count", {})
    assert status == 200, f"POST count failed: {body}"
    assert body.get("status") == "ok"
    assert body.get("result", {}).get("count") == 0

    # 5. Search with vector
    search_body = {
        "vector": [0.1, 0.2, -0.3, 0.4],
        "limit": 5,
    }
    status, body = daemon.post(f"/qdrant/collections/{col_name}/points/search", search_body)
    assert status == 200, f"POST search failed: {body}"
    assert body.get("status") == "ok"
    assert isinstance(body.get("result"), list)

    # 6. Delete collection
    status, body, _ = daemon.http("DELETE", f"/qdrant/collections/{col_name}")
    assert status == 200, f"DELETE failed: {body}"
    assert body.get("status") == "ok"
    assert body.get("result") is True

    # 7. Verify collection gone
    status, body = daemon.get(f"/qdrant/collections/{col_name}")
    assert status == 404
