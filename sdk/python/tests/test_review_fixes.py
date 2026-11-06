"""Unit tests verifying review recommendations:
idempotent read retries and reserved attribute filtering.
"""

from arkivector import (
    Client,
    Collection,
    ColumnDef,
    EntityInput,
    IndexDef,
    Vector,
    build_entity_attributes,
)


def test_build_entity_attributes_filters_reserved() -> None:
    col = Collection(
        name="test_col",
        schema_tag="arkivector.v1.test_col",
        column=ColumnDef("vector", 3),
        index=IndexDef("hnsw", "vector_l2_ops"),
        writers=["0x1111111111111111111111111111111111111111"],
        membership="creator",
        attributes={"title": "str"},
        default_ttl=None,
        open=False,
        manifest_key=None,
        stats={},
    )
    # Input has reserved keys in attributes (e.g. from prior entity search results)
    entity = EntityInput(
        id="doc-1",
        vector=Vector([1.0, 2.0, 3.0]),
        document="Sample document",
        attributes={
            "arkiv.schema": "rogue.schema",
            "av.id": "rogue-id",
            "av.doc": "rogue-doc",
            "av.vec": "rogue-vec",
            "title": "Legit Title",
        },
    )
    attrs = build_entity_attributes(col, entity)
    names = [a.name for a in attrs]

    assert names.count("arkiv.schema") == 1
    assert names.count("av.vec") == 1
    assert names.count("av.id") == 1
    assert names.count("av.doc") == 1
    assert names.count("title") == 1
    # Check that rogue attributes did not produce extra attributes
    assert len(attrs) == 5
    title_attr = next(a for a in attrs if a.name == "title")
    assert title_attr.value_bytes == b"Legit Title"


def test_idempotent_read_retries_configured() -> None:
    client = Client(url="http://127.0.0.1:9999", max_retries=2)
    # Verify _http recognizes search, scroll, count as idempotent paths
    assert client._http.can_retry(
        "POST", "/v1/collections/test/search", is_idempotent=True, idempotency_key=None
    )
    assert client._http.can_retry(
        "POST", "/v1/collections/test/scroll", is_idempotent=True, idempotency_key=None
    )
    assert client._http.can_retry(
        "POST", "/v1/collections/test/count", is_idempotent=True, idempotency_key=None
    )
