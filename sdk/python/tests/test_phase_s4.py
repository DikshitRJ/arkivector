"""Unit tests for Phase S4 Python SDK: writes, session consistency, and attribute builder."""

import uuid
from decimal import Decimal
from typing import Any, Dict, List, Optional

import pytest

from arkivector import (
    Client,
    Collection,
    ColumnDef,
    DirectAttribute,
    EntityInput,
    F,
    HalfVector,
    IndexDef,
    NotImplementedByDaemon,
    PreparedTransaction,
    PrepareResult,
    SparseVector,
    T,
    Ttl,
    TxResult,
    TypedValue,
    UpsertItemResult,
    UpsertResult,
    Vector,
    build_entity_attributes,
)

# ============================================================================
# Dummy response helper for mocking
# ============================================================================


class DummyResponse:
    def __init__(self, data: Dict[str, Any], headers: Optional[Dict[str, str]] = None) -> None:
        self.data = data
        self.headers = headers or {}
        self.body = b""

    def json(self) -> Dict[str, Any]:
        return self.data


# ============================================================================
# 1. Models tests
# ============================================================================


def test_entity_input_to_json_and_from_json() -> None:
    vec = Vector([0.1, -0.2, 0.3])
    ttl = Ttl.days(30)
    ei = EntityInput(
        id="doc-100",
        vector=vec,
        document="A finance doc",
        attributes={"category": "finance", "score": T.dec("4.5"), "rank": 10},
        ttl=ttl,
    )

    d = ei.to_json()
    assert d["id"] == "doc-100"
    assert d["vector"] == [pytest.approx(x) for x in [0.1, -0.2, 0.3]]
    assert d["document"] == "A finance doc"
    assert d["attributes"]["category"] == "finance"
    assert d["attributes"]["score"] == {"type": "dec", "value": "4.5"}
    assert d["attributes"]["rank"] == 10
    assert d["ttl"] == {"days": 30}

    # Deserialization from JSON
    ei2 = EntityInput.from_json(d)
    assert ei2.id == "doc-100"
    assert ei2.document == "A finance doc"
    assert ei2.attributes is not None
    assert isinstance(ei2.attributes["score"], TypedValue)
    assert ei2.attributes["score"].value == "4.5"
    assert isinstance(ei2.ttl, Ttl)
    assert ei2.ttl.kind == "days"
    assert ei2.ttl.value == 30


def test_entity_input_vector_types() -> None:
    # HalfVector
    ei_half = EntityInput(id="doc-1", vector=HalfVector([0.5, -0.5]))
    assert ei_half.to_json()["vector"] == [0.5, -0.5]

    # SparseVector
    ei_sparse = EntityInput(id="doc-2", vector=SparseVector(10, [1, 4], [1.5, -2.0]))
    assert ei_sparse.to_json()["vector"] == "{2:1.5,5:-2}/10"

    # Raw vector_bytes
    ei_b64 = EntityInput(id="doc-3", vector_bytes="QVYS...")
    d = ei_b64.to_json()
    assert "vector" not in d
    assert d["vector_bytes"] == "QVYS..."


def test_upsert_models() -> None:
    item = UpsertItemResult.from_json({"id": "doc-900", "key": "0x5e1f0c0d", "op": "create"})
    assert item.id == "doc-900"
    assert item.key == "0x5e1f0c0d"
    assert item.op == "create"
    assert item.to_json() == {"id": "doc-900", "key": "0x5e1f0c0d", "op": "create"}

    res = UpsertResult.from_json(
        {
            "results": [{"id": "doc-900", "key": "0x5e1f0c0d", "op": "create"}],
            "tx_hashes": ["0x7d1c3b5a"],
            "block_number": 582160,
        }
    )
    assert len(res.results) == 1
    assert res.results[0].id == "doc-900"
    assert res.tx_hashes == ["0x7d1c3b5a"]
    assert res.block_number == 582160
    assert res.to_json()["block_number"] == 582160


def test_prepare_models() -> None:
    tx = PreparedTransaction.from_json(
        {
            "to": "0x4400000000000000000000000000000000000044",
            "data": "0x49650044",
            "value": "0x0",
            "nonce": 12,
            "gas_estimate": 412000,
        }
    )
    assert tx.to == "0x4400000000000000000000000000000000000044"
    assert tx.data == "0x49650044"
    assert tx.value == "0x0"
    assert tx.nonce == 12
    assert tx.gas_estimate == 412000

    prep = PrepareResult.from_json(
        {
            "chain_id": 7794095,
            "transactions": [tx.to_json()],
            "results": [{"id": "doc-900", "key": "0x5e1f", "op": "create"}],
        }
    )
    assert prep.chain_id == 7794095
    assert len(prep.transactions) == 1
    assert len(prep.results) == 1


def test_tx_result_model() -> None:
    tx = TxResult.from_json(
        {
            "affected": 3,
            "tx_hashes": ["0xabc"],
            "block_number": 582170,
        }
    )
    assert tx.affected == 3
    assert tx.tx_hashes == ["0xabc"]
    assert tx.block_number == 582170
    assert tx.to_json()["affected"] == 3


def test_direct_attribute_namedtuple() -> None:
    da = DirectAttribute("av.vec", 0x07, b"bytes-payload")
    assert da.name == "av.vec"
    assert da.type_id == 0x07
    assert da.value_bytes == b"bytes-payload"

    # Tuple unpacking
    name, type_id, val = da
    assert name == "av.vec"
    assert type_id == 0x07
    assert val == b"bytes-payload"


# ============================================================================
# 2. build_entity_attributes tests
# ============================================================================


def test_build_entity_attributes_layout_and_ordering() -> None:
    col = Collection(
        name="demo.docs",
        schema_tag="arkivector.v1.demo.docs",
        column=ColumnDef(type="halfvec", dimensions=4),
        index=IndexDef(method="hnsw", opclass="halfvec_cosine_ops"),
        writers=[],
        membership="creator",
        attributes={"score": "dec", "rank": "i32", "published": "bool"},
        default_ttl=None,
        open=False,
        manifest_key=None,
        stats={},
    )

    entity = EntityInput(
        id="doc-42",
        vector=[0.1, -0.2, 0.3, -0.4],
        document="Sample document text.",
        attributes={
            "score": "4.5",
            "rank": 17,
            "published": True,
            "category": "finance",
        },
    )

    attrs = build_entity_attributes(col, entity)

    # 1. arkiv.schema (0x88)
    assert attrs[0].name == "arkiv.schema"
    assert attrs[0].type_id == 0x88
    assert attrs[0].value_bytes == b"arkivector.v1.demo.docs"

    # 2. av.vec (0x07)
    assert attrs[1].name == "av.vec"
    assert attrs[1].type_id == 0x07
    # 4-dim halfvec: header (8 bytes) + 4 binary16 floats (8 bytes) = 16 bytes
    assert len(attrs[1].value_bytes) == 16

    # 3. av.id (0x88)
    assert attrs[2].name == "av.id"
    assert attrs[2].type_id == 0x88
    assert attrs[2].value_bytes == b"doc-42"

    # 4. av.doc (0x07)
    assert attrs[3].name == "av.doc"
    assert attrs[3].type_id == 0x07
    assert attrs[3].value_bytes == b"Sample document text."

    # User attributes sorted by name: category, published, rank, score
    user_names = [a.name for a in attrs[4:]]
    assert user_names == ["category", "published", "rank", "score"]

    cat_attr = attrs[4]
    assert cat_attr.type_id == 0x08  # str
    assert cat_attr.value_bytes == b"finance"

    pub_attr = attrs[5]
    assert pub_attr.type_id == 0x01  # bool
    assert pub_attr.value_bytes == (1).to_bytes(32, "big")

    rank_attr = attrs[6]
    assert rank_attr.type_id == 0x02  # i32
    assert rank_attr.value_bytes == (17).to_bytes(32, "big", signed=True)

    score_attr = attrs[7]
    assert score_attr.type_id == 0x05  # dec scaled by 10^18
    expected_dec = int(Decimal("4.5") * Decimal(10**18)).to_bytes(32, "big", signed=True)
    assert score_attr.value_bytes == expected_dec


def test_build_entity_attributes_omits_optional_id_and_doc() -> None:
    col = {
        "name": "raw.col",
        "schema_tag": "tag1",
        "column": {"type": "vector", "dimensions": 2},
    }
    entity = {
        "vector": [1.0, 2.0],
    }

    attrs = build_entity_attributes(col, entity)
    names = [a.name for a in attrs]
    assert names == ["arkiv.schema", "av.vec"]
    assert attrs[0].type_id == 0x88
    assert attrs[1].type_id == 0x07


# ============================================================================
# 3. Client write operations tests
# ============================================================================


def test_client_upsert_payload_and_idempotency() -> None:
    client = Client(url="http://127.0.0.1:8080", session_consistency=True)
    client.has_capability = lambda name: True  # type: ignore[assignment]

    recorded_calls: List[Any] = []

    def mock_request(method: str, path: str, **kwargs: Any) -> DummyResponse:
        recorded_calls.append((method, path, kwargs))
        return DummyResponse(
            {
                "results": [{"id": "doc-1", "key": "0x5e1f", "op": "create"}],
                "tx_hashes": ["0x7d1c"],
                "block_number": 582160,
            }
        )

    client._http.request = mock_request  # type: ignore[assignment]

    res = client.upsert(
        "demo.docs",
        entities=[
            EntityInput(
                id="doc-1",
                vector=[0.1, -0.2],
                attributes={"score": T.dec("4.5")},
            )
        ],
        wait="receipt",
    )

    assert res.block_number == 582160
    assert client.last_write_block == 582160
    assert client.last_written_block == 582160

    call = recorded_calls[-1]
    assert call[0] == "POST"
    assert call[1] == "/v1/collections/demo.docs/entities"
    # Idempotency key was auto-generated UUIDv4
    idempotency_key = call[2]["idempotency_key"]
    assert idempotency_key is not None
    uuid.UUID(idempotency_key)  # Validates UUID format


def test_client_prepare() -> None:
    client = Client(url="http://127.0.0.1:8080")
    client.has_capability = lambda name: True  # type: ignore[assignment]

    recorded_calls: List[Any] = []

    def mock_request(method: str, path: str, **kwargs: Any) -> DummyResponse:
        recorded_calls.append((method, path, kwargs))
        return DummyResponse(
            {
                "chain_id": 7794095,
                "transactions": [
                    {
                        "to": "0x44",
                        "data": "0x49",
                        "value": "0x0",
                        "nonce": 1,
                        "gas_estimate": 100,
                    }
                ],
                "results": [{"id": "doc-1", "key": "0x12", "op": "create"}],
            }
        )

    client._http.request = mock_request  # type: ignore[assignment]

    prep = client.prepare(
        "demo.docs",
        entities=[EntityInput(id="doc-1", vector=[0.1, 0.2])],
        from_address="0x1111111111111111111111111111111111111111",
    )

    assert prep.chain_id == 7794095
    assert len(prep.transactions) == 1
    call = recorded_calls[-1]
    assert call[0] == "POST"
    assert call[1] == "/v1/collections/demo.docs/entities:prepare"
    assert call[2]["body"]["from"] == "0x1111111111111111111111111111111111111111"


def test_client_delete_single_ref_vs_batch() -> None:
    client = Client(url="http://127.0.0.1:8080", session_consistency=True)
    client.has_capability = lambda name: True  # type: ignore[assignment]

    recorded_calls: List[Any] = []

    def mock_request(method: str, path: str, **kwargs: Any) -> DummyResponse:
        recorded_calls.append((method, path, kwargs))
        return DummyResponse(
            {
                "affected": 1,
                "tx_hashes": ["0x123"],
                "block_number": 582170,
            }
        )

    client._http.request = mock_request  # type: ignore[assignment]

    # Single ref uses DELETE /v1/collections/{name}/entities/{ref}
    client.delete("demo.docs", refs=["doc-single"])
    assert recorded_calls[-1][0] == "DELETE"
    assert recorded_calls[-1][1] == "/v1/collections/demo.docs/entities/doc-single"
    assert client.last_write_block == 582170

    # Batch refs uses POST /v1/collections/{name}/entities:delete
    client.delete("demo.docs", refs=["doc-1", "doc-2"])
    assert recorded_calls[-1][0] == "POST"
    assert recorded_calls[-1][1] == "/v1/collections/demo.docs/entities:delete"
    assert recorded_calls[-1][2]["body"] == {"refs": ["doc-1", "doc-2"]}

    # Filter delete
    client.delete("demo.docs", filter=F.eq("category", "finance"))
    assert recorded_calls[-1][0] == "POST"
    assert recorded_calls[-1][1] == "/v1/collections/demo.docs/entities:delete"
    assert "filter" in recorded_calls[-1][2]["body"]

    # Multiple targets error
    with pytest.raises(ValueError, match="exactly one"):
        client.delete("demo.docs", refs=["doc-1"], where="category = 'finance'")


def test_client_extend() -> None:
    client = Client(url="http://127.0.0.1:8080", session_consistency=True)
    client.has_capability = lambda name: True  # type: ignore[assignment]

    recorded_calls: List[Any] = []

    def mock_request(method: str, path: str, **kwargs: Any) -> DummyResponse:
        recorded_calls.append((method, path, kwargs))
        return DummyResponse(
            {
                "affected": 2,
                "tx_hashes": ["0x456"],
                "block_number": 582180,
            }
        )

    client._http.request = mock_request  # type: ignore[assignment]

    res = client.extend("demo.docs", ttl=Ttl.days(30), refs=["doc-1", "doc-2"])
    assert res.affected == 2
    assert client.last_write_block == 582180

    call = recorded_calls[-1]
    assert call[0] == "POST"
    assert call[1] == "/v1/collections/demo.docs/entities:extend"
    assert call[2]["body"]["ttl"] == {"days": 30}
    assert call[2]["body"]["refs"] == ["doc-1", "doc-2"]


def test_session_consistency_read_injection() -> None:
    client = Client(url="http://127.0.0.1:8080", session_consistency=True)
    client.has_capability = lambda name: True  # type: ignore[assignment]
    client.last_write_block = 582160

    recorded_calls: List[Any] = []

    def mock_request(method: str, path: str, **kwargs: Any) -> DummyResponse:
        recorded_calls.append((method, path, kwargs))
        return DummyResponse({"as_of_block": 582160, "results": [], "entity": {"key": "0x1"}})

    client._http.request = mock_request  # type: ignore[assignment]

    # 1. get injects min_block in query params
    client.get("demo.docs", "0x1")
    assert recorded_calls[-1][2]["query"]["min_block"] == 582160

    # 2. search injects consistency min_block in body
    client.search("demo.docs", vector=[0.1, 0.2])
    assert recorded_calls[-1][2]["body"]["consistency"]["min_block"] == 582160

    # 3. count injects consistency min_block in body
    client.count("demo.docs")
    assert recorded_calls[-1][2]["body"]["consistency"]["min_block"] == 582160

    # 4. Explicit min_block takes precedence
    client.search("demo.docs", vector=[0.1, 0.2], min_block=580000)
    assert recorded_calls[-1][2]["body"]["consistency"]["min_block"] == 580000


def test_client_write_capability_checks() -> None:
    client = Client(url="http://127.0.0.1:8080")
    client._version_info = client.version = lambda: None  # type: ignore[assignment]
    client.has_capability = lambda name: False  # type: ignore[assignment]

    with pytest.raises(NotImplementedByDaemon, match="entities.write"):
        client.upsert("demo.docs", entities=[EntityInput(id="doc-1", vector=[0.1])])

    with pytest.raises(NotImplementedByDaemon, match="entities.prepare"):
        client.prepare(
            "demo.docs", entities=[EntityInput(id="doc-1", vector=[0.1])], from_address="0x1"
        )

    with pytest.raises(NotImplementedByDaemon, match="entities.write"):
        client.delete("demo.docs", refs=["doc-1"])

    with pytest.raises(NotImplementedByDaemon, match="entities.write"):
        client.extend("demo.docs", ttl=Ttl.days(1), refs=["doc-1"])
