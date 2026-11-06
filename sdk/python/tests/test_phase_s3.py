"""Unit tests for Phase S3 Python SDK: filters, models, and client search/scroll/count."""

import json

import pytest

from arkivector import (
    BatchResult,
    Client,
    Consistency,
    CountResult,
    Entity,
    F,
    HalfVector,
    ScrollPage,
    SearchHit,
    SearchResult,
    T,
    Vector,
)

# ============================================================================
# Filter tests (F builder)
# ============================================================================


def test_filter_equality_scalars() -> None:
    # str scalar -> T.str
    f_str = F.eq("category", "finance")
    assert f_str.to_dict() == {"eq": {"category": {"type": "str", "value": "finance"}}}

    # bool scalar -> T.bool
    f_bool = F.eq("published", True)
    assert f_bool.to_dict() == {"eq": {"published": {"type": "bool", "value": True}}}

    # int scalar -> T.i32
    f_int = F.eq("rank", 42)
    assert f_int.to_dict() == {"eq": {"rank": {"type": "i32", "value": 42}}}


def test_filter_float_raises_type_error() -> None:
    # Float scalar must raise TypeError / ValueError explaining ambiguity
    with pytest.raises(TypeError, match="ambiguous in Arkiv"):
        F.eq("score", 4.5)

    with pytest.raises(TypeError, match="ambiguous in Arkiv"):
        F.gte("price", 19.99)

    with pytest.raises(TypeError, match="ambiguous in Arkiv"):
        F.between("val", 0.0, 1.0)


def test_filter_typed_values() -> None:
    f_dec = F.gte("score", T.dec("4.0"))
    assert f_dec.to_dict() == {"gte": {"score": {"type": "dec", "value": "4.0"}}}

    f_addr = F.eq("$creator", T.addr("0x1111111111111111111111111111111111111111"))
    assert f_addr.to_dict() == {
        "eq": {"$creator": {"type": "addr", "value": "0x1111111111111111111111111111111111111111"}}
    }

    f_key = F.ne(
        "$key", T.key("0x2222222222222222222222222222222222222222222222222222222222222222")
    )
    assert f_key.to_dict() == {
        "ne": {
            "$key": {
                "type": "key",
                "value": "0x2222222222222222222222222222222222222222222222222222222222222222",
            }
        }
    }


def test_filter_comparison_operators() -> None:
    assert F.ne("tag", "alpha").to_dict() == {"ne": {"tag": {"type": "str", "value": "alpha"}}}
    assert F.lt("rank", 10).to_dict() == {"lt": {"rank": {"type": "i32", "value": 10}}}
    assert F.lte("rank", 10).to_dict() == {"lte": {"rank": {"type": "i32", "value": 10}}}
    assert F.gt("rank", 10).to_dict() == {"gt": {"rank": {"type": "i32", "value": 10}}}
    assert F.gte("rank", 10).to_dict() == {"gte": {"rank": {"type": "i32", "value": 10}}}


def test_filter_between() -> None:
    f = F.between("rank", -100, 100)
    assert f.to_dict() == {
        "between": {"rank": [{"type": "i32", "value": -100}, {"type": "i32", "value": 100}]}
    }


def test_filter_startswith() -> None:
    f = F.startswith("region", "eu-")
    assert f.to_dict() == {"startswith": {"region": "eu-"}}

    with pytest.raises(TypeError, match="prefix must be a str"):
        F.startswith("region", 123)  # type: ignore[arg-type]


def test_filter_exists() -> None:
    f = F.exists("archived")
    assert f.to_dict() == {"exists": "archived"}


def test_filter_typeof() -> None:
    f = F.typeof("score", "dec")
    assert f.to_dict() == {"typeof": {"score": "dec"}}

    with pytest.raises(ValueError, match="Invalid attribute type"):
        F.typeof("score", "invalid_type")


def test_filter_and_or_not() -> None:
    f1 = F.eq("category", "finance")
    f2 = F.gte("score", T.dec("4.0"))

    # F.and_ with varargs
    f_and_var = F.and_(f1, f2)
    assert f_and_var.to_dict() == {
        "and": [
            {"eq": {"category": {"type": "str", "value": "finance"}}},
            {"gte": {"score": {"type": "dec", "value": "4.0"}}},
        ]
    }

    # F.and_ with list
    f_and_list = F.and_([f1, f2])
    assert f_and_list.to_dict() == f_and_var.to_dict()

    # Operator &
    assert (f1 & f2).to_dict() == f_and_var.to_dict()

    # F.or_ with varargs
    f_or_var = F.or_(f1, f2)
    assert f_or_var.to_dict() == {
        "or": [
            {"eq": {"category": {"type": "str", "value": "finance"}}},
            {"gte": {"score": {"type": "dec", "value": "4.0"}}},
        ]
    }

    # F.or_ with list
    f_or_list = F.or_([f1, f2])
    assert f_or_list.to_dict() == f_or_var.to_dict()

    # Operator |
    assert (f1 | f2).to_dict() == f_or_var.to_dict()

    # F.not_
    f_not = F.not_(f1)
    assert f_not.to_dict() == {"not": {"eq": {"category": {"type": "str", "value": "finance"}}}}

    # Operator ~
    assert (~f1).to_dict() == f_not.to_dict()

    # Empty children errors
    with pytest.raises(ValueError, match="at least one child"):
        F.and_()
    with pytest.raises(ValueError, match="at least one child"):
        F.or_()


def test_filter_filters_md_examples() -> None:
    # Equivalence examples from filters.md §5:
    # 1. category = str('finance')
    ex1 = F.eq("category", "finance")
    assert ex1.to_dict() == {"eq": {"category": {"type": "str", "value": "finance"}}}

    # 2. score >= dec(4.0) AND published = true
    ex2 = F.and_(F.gte("score", T.dec("4.0")), F.eq("published", True))
    assert ex2.to_dict() == {
        "and": [
            {"gte": {"score": {"type": "dec", "value": "4.0"}}},
            {"eq": {"published": {"type": "bool", "value": True}}},
        ]
    }

    # 3. rank BETWEEN -100 AND 100
    ex3 = F.between("rank", -100, 100)
    assert ex3.to_dict() == {
        "between": {"rank": [{"type": "i32", "value": -100}, {"type": "i32", "value": 100}]}
    }

    # 4. NOT EXISTS(archived) OR region STARTSWITH str('eu-')
    ex4 = F.or_(F.not_(F.exists("archived")), F.startswith("region", "eu-"))
    assert ex4.to_dict() == {
        "or": [{"not": {"exists": "archived"}}, {"startswith": {"region": "eu-"}}]
    }

    # 5. $creator = addr(0x1111111111111111111111111111111111111111)
    ex5 = F.eq("$creator", T.addr("0x1111111111111111111111111111111111111111"))
    assert ex5.to_dict() == {
        "eq": {"$creator": {"type": "addr", "value": "0x1111111111111111111111111111111111111111"}}
    }


# ============================================================================
# Models tests
# ============================================================================


def test_consistency_model() -> None:
    c = Consistency(min_block=582160, timeout_ms=4000)
    d = c.to_json()
    assert d == {"min_block": 582160, "timeout_ms": 4000}

    c2 = Consistency.from_json(d)
    assert c2.min_block == 582160
    assert c2.timeout_ms == 4000


def test_entity_model_and_plain_attributes() -> None:
    raw = {
        "key": "0x0b4f4d8b4fd9b7b1d0b6a0c7b8d2e1f3a4c5d6e7f8091a2b3c4d5e6f7a8b9c0d",
        "id": "doc-040",
        "owner": "0x1111111111111111111111111111111111111111",
        "creator": "0x1111111111111111111111111111111111111111",
        "created_at_block": 580000,
        "updated_at_block": 580000,
        "expires_at_block": 10000000,
        "attributes": {
            "category": {"type": "str", "value": "finance"},
            "score": {"type": "dec", "value": "4.12"},
            "published": {"type": "bool", "value": True},
            "rank": {"type": "i32", "value": 17},
        },
        "document": "Some finance text",
        "vector_bytes": HalfVector([0.1, 0.2]).to_vectorbytes().hex(),  # will test decode
    }
    # Encode vector bytes as base64
    vec_b64 = "QVYSAAgAAAAzuIY0eS8OOTO7NjPqukC2"  # valid 8-dim halfvec from demo-dataset
    raw["vector_bytes"] = vec_b64

    entity = Entity.from_json(raw)
    assert entity.id == "doc-040"
    assert entity.key == raw["key"]
    assert entity.document == "Some finance text"
    assert entity.plain_attributes() == {
        "category": "finance",
        "score": "4.12",
        "published": True,
        "rank": 17,
    }
    # Decoded vector
    assert entity.vector is not None
    assert isinstance(entity.vector, HalfVector)
    assert entity.vector.dim == 8


def test_search_hit_model() -> None:
    vec_b64 = "QVYSAAgAAAAzuIY0eS8OOTO7NjPqukC2"
    raw = {
        "key": "0x0b4f4d8b4fd9b7b1d0b6a0c7b8d2e1f3a4c5d6e7f8091a2b3c4d5e6f7a8b9c0d",
        "id": "doc-040",
        "distance": 0.1182,
        "score": 0.8818,
        "owner": "0x1111111111111111111111111111111111111111",
        "creator": "0x1111111111111111111111111111111111111111",
        "expires_at_block": 10000000,
        "attributes": {
            "category": {"type": "str", "value": "finance"},
        },
        "document": None,
        "vector_bytes": vec_b64,
    }
    hit = SearchHit.from_json(raw)
    assert hit.id == "doc-040"
    assert hit.distance == 0.1182
    assert hit.score == 0.8818
    assert hit.plain_attributes() == {"category": "finance"}
    assert hit.vector is not None
    assert isinstance(hit.vector, HalfVector)


def test_search_result_batch_scroll_count() -> None:
    res_dict = {
        "as_of_block": 582151,
        "sync_state": "live",
        "open_collection": False,
        "results": [
            {
                "key": "0x0b4f4d8b4fd9b7b1d0b6a0c7b8d2e1f3a4c5d6e7f8091a2b3c4d5e6f7a8b9c0d",
                "id": "doc-040",
                "distance": 0.1182,
                "score": 0.8818,
                "owner": "0x1111111111111111111111111111111111111111",
                "creator": "0x1111111111111111111111111111111111111111",
                "expires_at_block": 10000000,
                "attributes": {},
            }
        ],
        "plan": {"strategy": "exact_flat", "candidates": 46, "exact": True, "elapsed_ms": 0.21},
    }
    sr = SearchResult.from_json(res_dict)
    assert sr.as_of_block == 582151
    assert sr.sync_state == "live"
    assert len(sr.hits) == 1
    assert len(sr.results) == 1
    assert sr.plan is not None and sr.plan["strategy"] == "exact_flat"

    # BatchResult
    batch_dict = {
        "as_of_block": 582151,
        "sync_state": "live",
        "responses": [{"results": res_dict["results"]}],
    }
    br = BatchResult.from_json(batch_dict)
    assert br.as_of_block == 582151
    assert len(br.results) == 1
    assert len(br.results[0].hits) == 1

    # ScrollPage
    sp_dict = {
        "as_of_block": 582151,
        "entities": [
            {
                "key": "0x0b4f4d8b4fd9b7b1d0b6a0c7b8d2e1f3a4c5d6e7f8091a2b3c4d5e6f7a8b9c0d",
                "id": "doc-040",
                "owner": "0x1111111111111111111111111111111111111111",
                "creator": "0x1111111111111111111111111111111111111111",
                "created_at_block": 580000,
                "updated_at_block": 580000,
                "expires_at_block": 10000000,
                "attributes": {},
            }
        ],
        "next_cursor": "cursor123",
    }
    sp = ScrollPage.from_json(sp_dict)
    assert sp.as_of_block == 582151
    assert len(sp.entities) == 1
    assert sp.next_cursor == "cursor123"

    # CountResult
    cr = CountResult.from_json({"as_of_block": 582151, "count": 46})
    assert cr.as_of_block == 582151
    assert cr.count == 46


# ============================================================================
# Client vector serialization tests
# ============================================================================


def test_client_vector_serialization() -> None:
    client = Client(url="http://127.0.0.1:4010")

    # HalfVector with halfvec col_type -> sends vector_bytes base64
    hv = HalfVector([0.1, 0.2, 0.3])
    v_json, v_b64 = client._serialize_search_vector(hv, "halfvec")
    assert v_json is None
    assert v_b64 is not None

    # Vector with vector col_type -> sends vector_bytes base64
    vec = Vector([0.1, 0.2, 0.3])
    v_json, v_b64 = client._serialize_search_vector(vec, "vector")
    assert v_json is None
    assert v_b64 is not None

    # HalfVector with vector col_type (mismatch) -> sends JSON array
    v_json, v_b64 = client._serialize_search_vector(hv, "vector")
    assert v_b64 is None
    assert isinstance(v_json, list)
    assert len(v_json) == 3

    # List of floats -> sends JSON array
    v_json, v_b64 = client._serialize_search_vector([0.1, 0.2, 0.3], "halfvec")
    assert v_b64 is None
    assert v_json == [0.1, 0.2, 0.3]


# ============================================================================
# Client mock request tests
# ============================================================================


class DummyResponse:
    def __init__(self, data: dict, headers: dict = None) -> None:
        self.data = data
        self.headers = headers or {}
        self.body = json.dumps(data).encode("utf-8")

    def json(self) -> dict:
        return self.data


def test_client_get_path_encoding() -> None:
    client = Client(url="http://127.0.0.1:8080")
    client._version_info = client.version = lambda: None  # type: ignore[assignment]
    client.has_capability = lambda name: True  # type: ignore[assignment]

    recorded_calls = []

    def mock_request(method, path, **kwargs):
        recorded_calls.append((method, path, kwargs))
        return DummyResponse(
            {
                "entity": {
                    "key": "0x0b4f4d8b4fd9b7b1d0b6a0c7b8d2e1f3a4c5d6e7f8091a2b3c4d5e6f7a8b9c0d",
                    "id": "doc-040",
                    "owner": "0x1111111111111111111111111111111111111111",
                    "creator": "0x1111111111111111111111111111111111111111",
                    "created_at_block": 100,
                    "updated_at_block": 100,
                    "expires_at_block": 1000,
                    "attributes": {},
                }
            }
        )

    client._http.request = mock_request  # type: ignore[assignment]

    # Hex key should NOT be percent-encoded
    hex_key = "0x0b4f4d8b4fd9b7b1d0b6a0c7b8d2e1f3a4c5d6e7f8091a2b3c4d5e6f7a8b9c0d"
    client.get("demo.docs", hex_key)
    assert recorded_calls[-1][1] == f"/v1/collections/demo.docs/entities/{hex_key}"

    # Non-hex ref should BE percent-encoded
    client.get("demo.docs", "item/1 with spaces")
    assert recorded_calls[-1][1] == "/v1/collections/demo.docs/entities/item%2F1%20with%20spaces"


def test_client_search_payload() -> None:
    client = Client(url="http://127.0.0.1:8080")
    client.has_capability = lambda name: True  # type: ignore[assignment]
    client._collection_column_types["demo.docs"] = "halfvec"

    recorded_calls = []

    def mock_request(method, path, **kwargs):
        recorded_calls.append((method, path, kwargs))
        return DummyResponse(
            {
                "as_of_block": 582151,
                "sync_state": "live",
                "open_collection": False,
                "results": [],
            }
        )

    client._http.request = mock_request  # type: ignore[assignment]

    # Search with HalfVector matching column type
    hv = HalfVector([0.1, 0.2])
    client.search(
        "demo.docs",
        vector=hv,
        k=5,
        filter=F.eq("category", "finance"),
        consistency=Consistency(min_block=580000),
    )

    call = recorded_calls[-1]
    assert call[0] == "POST"
    assert call[1] == "/v1/collections/demo.docs/search"
    body = call[2]["body"]
    assert "vector_bytes" in body
    assert "vector" not in body
    assert body["k"] == 5
    assert body["filter"] == {"eq": {"category": {"type": "str", "value": "finance"}}}
    assert body["consistency"] == {"min_block": 580000}


def test_client_scroll_pagination() -> None:
    client = Client(url="http://127.0.0.1:8080")
    client.has_capability = lambda name: True  # type: ignore[assignment]

    pages = [
        DummyResponse(
            {
                "as_of_block": 100,
                "entities": [
                    {
                        "key": "0x1111111111111111111111111111111111111111111111111111111111111111",
                        "id": "e1",
                        "owner": "0x0",
                        "creator": "0x0",
                        "created_at_block": 1,
                        "updated_at_block": 1,
                        "expires_at_block": 10,
                        "attributes": {},
                    }
                ],
                "next_cursor": "cur2",
            }
        ),
        DummyResponse(
            {
                "as_of_block": 100,
                "entities": [
                    {
                        "key": "0x2222222222222222222222222222222222222222222222222222222222222222",
                        "id": "e2",
                        "owner": "0x0",
                        "creator": "0x0",
                        "created_at_block": 1,
                        "updated_at_block": 1,
                        "expires_at_block": 10,
                        "attributes": {},
                    }
                ],
                "next_cursor": None,
            }
        ),
    ]

    page_iter = iter(pages)
    client._http.request = lambda method, path, **kwargs: next(page_iter)  # type: ignore[assignment]

    entities = list(client.scroll("demo.docs", limit=1))
    assert len(entities) == 2
    assert entities[0].id == "e1"
    assert entities[1].id == "e2"
