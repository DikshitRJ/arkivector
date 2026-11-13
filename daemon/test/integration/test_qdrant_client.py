"""Qdrant compatibility writes and payloads with the official qdrant-client (GitHub issue #15).

upsert -> search (with payload) -> delete -> search against mock-arkiv, plus filters, count, UUID
ids, batch upserts, the nested-payload rules, and the read-only daemon (no signer) refusing writes.
"""

from __future__ import annotations

import time
import uuid

import pytest
from conftest import DaemonClient, DaemonFactory, MockArkiv

qdrant_client = pytest.importorskip("qdrant_client")
from qdrant_client import QdrantClient  # noqa: E402
from qdrant_client.http.exceptions import UnexpectedResponse  # noqa: E402
from qdrant_client.models import (  # noqa: E402
    Distance,
    FieldCondition,
    Filter,
    FilterSelector,
    MatchAny,
    MatchValue,
    PointIdsList,
    PointStruct,
    Range,
    VectorParams,
)


def client_for(d: DaemonClient) -> QdrantClient:
    return QdrantClient(url=d.url, prefix="qdrant", timeout=60, check_compatibility=False)


@pytest.fixture
def qd(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> tuple[DaemonClient, QdrantClient]:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    d = daemon_factory.start()
    return d, client_for(d)


POINTS = [
    PointStruct(id=1, vector=[1.0, 0.0, 0.0, 0.0], payload={"page_content": "Cats purr.", "topic": "pets", "stars": 5, "rating": 4.5}),
    PointStruct(id=2, vector=[0.9, 0.1, 0.0, 0.0], payload={"page_content": "Dogs bark.", "topic": "pets", "stars": 4, "rating": 3.0}),
    PointStruct(id=3, vector=[0.0, 0.0, 1.0, 0.0], payload={"page_content": "Cars need fuel.", "topic": "vehicles", "stars": 3}),
]


def test_upsert_search_delete_search(qd: tuple[DaemonClient, QdrantClient]) -> None:
    d, c = qd
    assert c.create_collection("docs", vectors_config=VectorParams(size=4, distance=Distance.COSINE))

    res = c.upsert("docs", points=POINTS, wait=True)
    assert res.status.value == "completed"
    assert c.count("docs").count == 3

    hits = c.query_points("docs", query=[1.0, 0.05, 0.0, 0.0], limit=2, with_payload=True).points
    assert [h.id for h in hits] == [1, 2]
    assert hits[0].payload == {"page_content": "Cats purr.", "topic": "pets", "stars": 5, "rating": 4.5}
    assert hits[0].score > hits[1].score

    # payload selection, vectors, filters
    hits = c.query_points("docs", query=[1.0, 0.0, 0.0, 0.0], limit=3, with_payload=["topic"], with_vectors=True).points
    assert hits[0].payload == {"topic": "pets"}
    assert hits[0].vector == pytest.approx([1.0, 0.0, 0.0, 0.0], abs=1e-3)
    flt = Filter(must=[FieldCondition(key="topic", match=MatchValue(value="vehicles"))])
    hits = c.query_points("docs", query=[1.0, 0.0, 0.0, 0.0], limit=3, query_filter=flt, with_payload=False).points
    assert [h.id for h in hits] == [3]
    assert hits[0].payload is None
    # Qdrant's REST default (no with_payload) is no payload
    status, body = d.post("/qdrant/collections/docs/points/search", {"vector": [0, 0, 1, 0], "limit": 1})
    assert status == 200 and body["result"][0]["id"] == 3 and body["result"][0]["payload"] is None
    flt = Filter(must=[FieldCondition(key="stars", range=Range(gte=4))], must_not=[FieldCondition(key="topic", match=MatchAny(any=["vehicles"]))])
    assert {h.id for h in c.query_points("docs", query=[0, 0, 1.0, 0], limit=3, query_filter=flt).points} == {1, 2}
    flt = Filter(must=[FieldCondition(key="rating", range=Range(gt=3.5))])
    assert [h.id for h in c.query_points("docs", query=[0, 0, 1.0, 0], limit=3, query_filter=flt).points] == [1]
    assert c.count("docs", count_filter=Filter(must=[FieldCondition(key="topic", match=MatchValue(value="pets"))])).count == 2

    # upsert of an existing id updates it in place
    c.upsert("docs", points=[PointStruct(id=2, vector=[0.9, 0.1, 0.0, 0.0], payload={"page_content": "Dogs bark loudly.", "topic": "pets", "stars": 2})], wait=True)
    hits = c.query_points("docs", query=[0.9, 0.1, 0.0, 0.0], limit=1, with_payload=True).points
    assert hits[0].id == 2 and hits[0].payload["page_content"] == "Dogs bark loudly." and hits[0].payload["stars"] == 2
    assert c.count("docs").count == 3

    # delete by id (unknown ids are ignored, like Qdrant)
    res = c.delete("docs", points_selector=PointIdsList(points=[1, 999]), wait=True)
    assert res.status.value == "completed"
    hits = c.query_points("docs", query=[1.0, 0.05, 0.0, 0.0], limit=3, with_payload=True).points
    assert [h.id for h in hits] == [2, 3]

    # delete by filter
    c.delete("docs", points_selector=FilterSelector(filter=Filter(must=[FieldCondition(key="topic", match=MatchValue(value="vehicles"))])), wait=True)
    assert [h.id for h in c.query_points("docs", query=[0, 0, 1.0, 0], limit=3).points] == [2]
    assert c.count("docs").count == 1


def test_uuid_ids_batch_and_nested_payloads(qd: tuple[DaemonClient, QdrantClient]) -> None:
    d, c = qd
    c.create_collection("lc", vectors_config=VectorParams(size=3, distance=Distance.EUCLID))
    u1, u2 = str(uuid.uuid4()), uuid.uuid4().hex
    # LangChain-style point: page_content + nested metadata (dropped, Warning header)
    status, body, headers = d.http(
        "PUT",
        "/qdrant/collections/lc/points?wait=true",
        {"points": [{"id": u1, "vector": [1, 0, 0], "payload": {"page_content": "hello", "metadata": {"source": "a.txt"}}}]},
    )
    assert status == 200, body
    assert body["result"]["status"] == "completed"
    assert "dropped" in (headers.get("Warning") or "")
    # no text key: nested and non-attribute keys are kept as a JSON document and merged back
    status, body, headers = d.http(
        "PUT",
        "/qdrant/collections/lc/points?wait=true",
        {"batch": {"ids": [u2], "vectors": [[0, 1, 0]], "payloads": [{"_node_content": "{\"x\":1}", "meta": {"k": [1, 2]}, "tag": "t"}]}},
    )
    assert status == 200, body
    assert headers.get("Warning") is None
    hits = c.query_points("lc", query=[0, 1, 0], limit=2, with_payload=True).points
    assert hits[0].id == str(uuid.UUID(u2))
    assert hits[0].score == pytest.approx(0.0, abs=1e-3)  # Euclid: the score is the distance
    assert hits[0].payload == {"tag": "t", "_node_content": "{\"x\":1}", "meta": {"k": [1, 2]}}
    assert hits[1].id == u1 and hits[1].payload == {"page_content": "hello"}

    # without ?wait the upsert is acknowledged, then becomes searchable
    status, body = d.http("PUT", "/qdrant/collections/lc/points", {"points": [{"id": 7, "vector": [0, 0, 1]}]})[:2]
    assert status == 200 and body["result"]["status"] == "acknowledged"
    deadline = time.time() + 10
    while time.time() < deadline and c.count("lc").count < 3:
        time.sleep(0.1)
    assert c.count("lc").count == 3

    # bad input is a Qdrant-shaped error, never a fake success
    status, body = d.http("PUT", "/qdrant/collections/lc/points", {"points": [{"id": -1, "vector": [0, 0, 1]}]})[:2]
    assert status == 422 and "error" in body["status"]
    status, body = d.http("PUT", "/qdrant/collections/lc/points", {"points": [{"id": 8, "vector": {"text": [1, 2, 3]}}]})[:2]
    assert status == 501 and "error" in body["status"]
    status, body = d.http("POST", "/qdrant/collections/lc/points/search", {"vector": [0, 0, 1], "filter": {"must": [{"key": "a.b", "match": {"value": 1}}]}})[:2]
    assert status == 501, body


def test_read_only_daemon_refuses_writes(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    d = daemon_factory.start(signer=False)
    c = client_for(d)
    c.create_collection("ro", vectors_config=VectorParams(size=2, distance=Distance.DOT))
    with pytest.raises(UnexpectedResponse) as ei:
        c.upsert("ro", points=[PointStruct(id=1, vector=[1.0, 0.0])], wait=True)
    assert ei.value.status_code == 403
    with pytest.raises(UnexpectedResponse) as ei:
        c.delete("ro", points_selector=PointIdsList(points=[1]))
    assert ei.value.status_code == 403
    status, body = d.get("/qdrant/collections/ro")
    assert body["result"]["points_count"] == 0
    status, body = d.get("/qdrant/")
    assert status == 200 and body["version"].startswith("1.")
