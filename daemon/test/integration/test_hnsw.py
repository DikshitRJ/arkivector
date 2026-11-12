"""Integration tests for the HNSW index path (collections with "index": {"method": "hnsw"}).

The planner picks HNSW only for large candidate sets (> 2048 and above gamma*sqrt(m*ef*N)), so each
test seeds a few thousand entities into mock-arkiv and compares approximate results with exact ones.

Covers:
- the plan really is hnsw / hnsw_iterative (not a silent flat scan)
- recall@10 against exact search
- reported distances are the operator's distances (not pgvector's index distances: L2 squared,
  negative inner product) and come back ascending
- iterative_scan off / relaxed_order / strict_order with a filter (off used to dereference NULL)
- zero-norm vectors under cosine (skipped by the graph, never NaN in JSON)
- deleted and replaced entities never appear in approximate results
- ef_search / iterative_scan / max_scan_tuples validation
"""

from __future__ import annotations

import math
import random
import struct
import time
from typing import Any

import pytest
from conftest import DEFAULT_OWNER, DaemonClient, MockArkiv

pytestmark = pytest.mark.hnsw

DIM = 16
N = 3000


def enc_vector(vals: list[float]) -> str:
    """VectorBytes v1, type vector (spec/tools/gen_golden.py enc_vector)."""
    raw = b"AV" + bytes([(1 << 4) | 1, 0]) + struct.pack("<hh", len(vals), 0) + struct.pack(f"<{len(vals)}f", *vals)
    return "0x" + raw.hex()


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def entity(schema: str, vec: list[float], category: str) -> dict[str, Any]:
    return {
        "attributes": [
            {"name": "arkiv.schema", "type": "str", "value": schema, "indexed": True},
            {"name": "av.vec", "type": "bytes", "value": enc_vector(vec), "indexed": False},
            {"name": "category", "type": "str", "value": category, "indexed": True},
        ],
    }


def l2(a: list[float], b: list[float]) -> float:
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def neg_ip(a: list[float], b: list[float]) -> float:
    return -sum(x * y for x, y in zip(a, b))


def make_collection(
    daemon: DaemonClient, mock: MockArkiv, name: str, opclass: str, vectors: list[list[float]], cats: list[str]
) -> dict[str, str]:
    """Create the collection, seed the entities, wait until the daemon has synced them. Returns key -> index."""
    schema = f"arkivector.v1.test.{name}"
    created = mock.ctl("POST", "/entities", [entity(schema, v, c) for v, c in zip(vectors, cats)])["created"]
    keys = [k for r in created for k in r["keys"]]
    assert len(keys) == len(vectors)
    mock.mine(1)
    col = {
        "name": name,
        "schema_tag": schema,
        "column": {"type": "vector", "dimensions": DIM},
        "index": {"method": "hnsw", "opclass": opclass, "with": {"m": 16, "ef_construction": 64}},
        "writers": [DEFAULT_OWNER],
        "membership": "creator",
        "attributes": {"category": "str"},
    }
    status, body = daemon.post("/v1/collections", col)
    assert status in (200, 201), body
    wait_synced(daemon, mock, name, len(vectors))
    return {k: str(i) for i, k in enumerate(keys)}


def wait_synced(daemon: DaemonClient, mock: MockArkiv, name: str, live: int) -> None:
    head = mock.head()
    deadline = time.time() + 120
    last: Any = None
    while time.time() < deadline:
        status, body = daemon.get(f"/v1/collections/{name}")
        last = body
        if status == 200 and body.get("sync", {}).get("as_of_block", 0) >= head and body.get("stats", {}).get("live_entities") == live:
            return
        time.sleep(0.2)
    raise TimeoutError(f"{name} did not sync {live} entities: {last}")


def search(daemon: DaemonClient, name: str, q: list[float], **extra: Any) -> dict[str, Any]:
    status, body = daemon.post(f"/v1/collections/{name}/search", {"vector": q, "k": 10, "explain": True, **extra})
    assert status == 200, body
    return body


def gaussian_vectors(rng: random.Random, n: int) -> list[list[float]]:
    return [[f32(rng.gauss(0.0, 1.0)) for _ in range(DIM)] for _ in range(n)]


def test_hnsw_l2_plan_recall_and_true_distances(daemon: DaemonClient, mock_arkiv: MockArkiv) -> None:
    rng = random.Random(11)
    vecs = gaussian_vectors(rng, N)
    keys = make_collection(daemon, mock_arkiv, "hnsw_l2", "vector_l2_ops", vecs, ["a"] * N)
    total_recall = 0.0
    queries = gaussian_vectors(rng, 20)
    for q in queries:
        approx = search(daemon, "hnsw_l2", q)
        assert approx["plan"]["strategy"] == "hnsw", approx["plan"]
        assert approx["plan"]["exact"] is False
        exact = search(daemon, "hnsw_l2", q, exact=True)
        assert exact["plan"]["strategy"] == "exact_flat"
        hits = approx["results"]
        assert len(hits) == 10
        dists = [h["distance"] for h in hits]
        assert dists == sorted(dists)
        for h in hits:
            want = l2(q, vecs[int(keys[h["key"]])])
            # operator distance (sqrt), not L2 squared from the graph
            assert h["distance"] == pytest.approx(want, rel=1e-4, abs=1e-5), (h, want)
        exact_keys = {h["key"] for h in exact["results"]}
        total_recall += len(exact_keys & {h["key"] for h in hits}) / 10.0
    assert total_recall / len(queries) >= 0.9


def test_hnsw_filtered_iterative_modes(daemon: DaemonClient, mock_arkiv: MockArkiv) -> None:
    rng = random.Random(12)
    vecs = gaussian_vectors(rng, N)
    cats = ["a" if rng.random() < 0.9 else "b" for _ in range(N)]
    keys = make_collection(daemon, mock_arkiv, "hnsw_filt", "vector_l2_ops", vecs, cats)
    q = gaussian_vectors(rng, 1)[0]
    flt = {"eq": {"category": "a"}}
    exact = search(daemon, "hnsw_filt", q, filter=flt, exact=True)
    exact_keys = {h["key"] for h in exact["results"]}
    for mode in ("off", "relaxed_order", "strict_order"):
        body = search(daemon, "hnsw_filt", q, filter=flt, iterative_scan=mode, ef_search=100)
        assert body["plan"]["strategy"] == "hnsw_iterative", (mode, body["plan"])
        hits = body["results"]
        assert len(hits) == 10, (mode, body)
        for h in hits:
            assert cats[int(keys[h["key"]])] == "a"
        dists = [h["distance"] for h in hits]
        assert dists == sorted(dists)
        assert len(exact_keys & {h["key"] for h in hits}) >= 8, mode


def test_hnsw_inner_product_negative_distances(daemon: DaemonClient, mock_arkiv: MockArkiv) -> None:
    rng = random.Random(13)
    vecs = gaussian_vectors(rng, N)
    keys = make_collection(daemon, mock_arkiv, "hnsw_ip", "vector_ip_ops", vecs, ["a"] * N)
    q = gaussian_vectors(rng, 1)[0]
    for mode in ("relaxed_order", "strict_order"):
        body = search(daemon, "hnsw_ip", q, iterative_scan=mode)
        assert body["plan"]["strategy"] == "hnsw"
        hits = body["results"]
        assert len(hits) == 10
        for h in hits:
            assert h["distance"] == pytest.approx(neg_ip(q, vecs[int(keys[h["key"]])]), rel=1e-4, abs=1e-4)
            assert h["distance"] < 0
        assert [h["distance"] for h in hits] == sorted(h["distance"] for h in hits)


def test_hnsw_cosine_zero_vectors(daemon: DaemonClient, mock_arkiv: MockArkiv) -> None:
    rng = random.Random(14)
    vecs = gaussian_vectors(rng, N)
    for i in range(0, 50):
        vecs[i] = [0.0] * DIM  # not indexable under cosine: kept out of the graph, never NaN in JSON
    make_collection(daemon, mock_arkiv, "hnsw_cos", "vector_cosine_ops", vecs, ["a"] * N)
    q = gaussian_vectors(rng, 1)[0]
    body = search(daemon, "hnsw_cos", q)
    assert body["plan"]["strategy"] == "hnsw"
    assert len(body["results"]) == 10
    for h in body["results"]:
        assert 0.0 <= h["distance"] <= 2.0
    # a zero query has no cosine neighbours; the request must still succeed
    status, zero = daemon.post("/v1/collections/hnsw_cos/search", {"vector": [0.0] * DIM, "k": 5})
    assert status == 200, zero


def test_hnsw_deleted_and_replaced_entities(daemon: DaemonClient, mock_arkiv: MockArkiv) -> None:
    rng = random.Random(15)
    vecs = gaussian_vectors(rng, N)
    keys = make_collection(daemon, mock_arkiv, "hnsw_upd", "vector_l2_ops", vecs, ["a"] * N)
    q = gaussian_vectors(rng, 1)[0]
    first = search(daemon, "hnsw_upd", q)["results"]
    gone = [h["key"] for h in first[:5]]
    for k in gone:
        mock_arkiv.ctl("DELETE", f"/entities/{k}")
    # move one entity right onto the query: it must come back first, with its new distance
    moved = first[5]["key"]
    schema = "arkivector.v1.test.hnsw_upd"
    patch = entity(schema, q, "a")
    mock_arkiv.ctl("PATCH", f"/entities/{moved}", {"attributes": patch["attributes"]})
    mock_arkiv.mine(1)
    wait_synced(daemon, mock_arkiv, "hnsw_upd", N - len(gone))
    body = search(daemon, "hnsw_upd", q)
    assert body["plan"]["strategy"] == "hnsw"
    hit_keys = [h["key"] for h in body["results"]]
    assert not set(gone) & set(hit_keys)
    assert hit_keys[0] == moved
    assert body["results"][0]["distance"] == pytest.approx(0.0, abs=1e-5)
    assert len(hit_keys) == 10
    del keys


def test_hnsw_knob_validation(daemon: DaemonClient, mock_arkiv: MockArkiv) -> None:
    rng = random.Random(16)
    make_collection(daemon, mock_arkiv, "hnsw_val", "vector_l2_ops", gaussian_vectors(rng, 20), ["a"] * 20)
    q = [0.5] * DIM
    for field, value in (
        ("ef_search", 0),
        ("ef_search", 1001),
        ("iterative_scan", "sideways"),
        ("max_scan_tuples", 0),
    ):
        status, body = daemon.post("/v1/collections/hnsw_val/search", {"vector": q, field: value})
        assert status == 422, (field, value, body)  # docs/api/errors.md: invalid_argument is 422
        assert body["code"] == "invalid_argument"
        assert body.get("field") == f"/{field}"
    status, body = daemon.post(
        "/v1/collections/hnsw_val/search",
        {"vector": q, "ef_search": 64, "iterative_scan": "strict_order", "max_scan_tuples": 5000},
    )
    assert status == 200, body
