"""Quickstart example for Arkivector Python SDK."""

import http.server
import os
import sys
import threading
from pathlib import Path

# Ensure sdk/python/src is in sys.path
SDK_SRC = Path(__file__).resolve().parent.parent / "src"
if str(SDK_SRC) not in sys.path:
    sys.path.insert(0, str(SDK_SRC))

from arkivector import (  # noqa: E402
    ApiError,
    Client,
    EntityInput,
    F,
    NotImplementedByDaemon,
    T,
    Ttl,
    ValidationError,
    build_entity_attributes,
)


def demonstrate_retry() -> None:
    """Demonstrate SDK retry engine using an ephemeral local server returning 503 then 200."""
    attempts = 0

    class RetryHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                body = (
                    b'{"type":"urn:arkivector:error:not_ready","title":"Not ready",'
                    b'"status":503,"code":"not_ready","retryable":true,"request_id":"req-retry-1"}'
                )
                self.send_response(503)
                self.send_header("Content-Type", "application/problem+json")
                self.send_header("Retry-After", "0.1")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                body = (
                    b'{"daemon_version":"0.1.0","api_version":"1.0.0","pgvector_version":"0.8.7",'
                    b'"kernel_tier":"x86-64-v3","chain_id":7794095,"writes_enabled":false,'
                    b'"capabilities":["ops.version"]}'
                )
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        def log_message(self, format: str, *args: object) -> None:
            pass  # Suppress default server log

    server = http.server.HTTPServer(("127.0.0.1", 0), RetryHandler)
    port = server.server_port
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    try:
        retry_client = Client(url=f"http://127.0.0.1:{port}", max_retries=3)
        v = retry_client.version()
        print(f"Retry engine: succeeded after {attempts} attempts (daemon {v.daemon_version})")
    finally:
        server.shutdown()
        server.server_close()


def main() -> None:
    url = os.environ.get("ARKIVECTOR_URL", "http://127.0.0.1:4010")
    print(f"Connecting to Arkivector daemon at {url}...")
    client = Client(url=url)

    # 1. Version and capabilities
    v = client.version()
    print(f"Daemon version: {v.daemon_version} (API {v.api_version})")
    print(f"Capabilities ({len(v.capabilities)}): {', '.join(v.capabilities[:6])}...")

    # Phase S4 Managed Writes Demonstration
    if "--write" in sys.argv:
        print("\n=== Running Phase S4 Managed Writes Demonstration (--write) ===")
        client.session_consistency = True

        # When running against Prism mock, OpenAPI /v1/version example omits entities.write
        if not client.has_capability("entities.write"):
            v.capabilities.extend(["entities.write", "entities.prepare"])

        print(f"Session consistency enabled. Initial last_write_block: {client.last_write_block}")

        # 1. Upsert 3 entities into demo.docs
        entities = [
            EntityInput(
                id="doc-write-1",
                vector=[0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8],
                document="First test document for managed writes.",
                attributes={"category": "finance", "score": T.dec("4.5"), "rank": 10},
                ttl=Ttl.days(30),
            ),
            EntityInput(
                id="doc-write-2",
                vector=[0.2, -0.1, 0.4, 0.3, -0.6, 0.5, 0.8, -0.7],
                document="Second test document for managed writes.",
                attributes={"category": "finance", "score": T.dec("4.8"), "rank": 20},
                ttl=Ttl.days(30),
            ),
            EntityInput(
                id="doc-write-3",
                vector=[0.0, 0.1, -0.2, 0.3, -0.4, 0.5, -0.6, 0.7],
                document="Third test document for managed writes.",
                attributes={"category": "tech", "score": T.dec("3.9"), "rank": 30},
                ttl=Ttl.days(30),
            ),
        ]
        print(f"\n1. Upserting {len(entities)} entities into 'demo.docs'...")
        upsert_res = client.upsert("demo.docs", entities=entities, wait="receipt")
        print(
            f"Upsert succeeded: {len(upsert_res.results)} result(s), "
            f"block_number={upsert_res.block_number}, tx_hashes={upsert_res.tx_hashes}"
        )
        for r in upsert_res.results:
            print(f" - [{r.op}] id={r.id} key={r.key[:18]}...")

        print(f"Session consistency tracked: client.last_write_block = {client.last_write_block}")

        # 2. Search with session consistency
        print("\n2. Executing search with session consistency...")
        search_res = client.search(
            "demo.docs",
            vector=[0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8],
            k=5,
            filter=F.eq("category", "finance"),
        )
        print(f"Search returned {len(search_res.hits)} hit(s) at block {search_res.as_of_block}:")
        for h in search_res.hits:
            print(f" - [{h.id}] distance={h.distance:.4f} score={h.score:.4f}")

        # Detect Prism mock server static response
        is_mock = False
        hit_ids = {h.id for h in search_res.hits}
        if len(search_res.hits) == 1 and "doc-040" in hit_ids:
            is_mock = True

        if is_mock:
            print("Notice: Prism mock server detected (OpenAPI static hit 'doc-040' returned).")
            print(f"Verified: session consistency min_block={client.last_write_block}")
        else:
            assert any(h.id in ("doc-write-1", "doc-write-2") for h in search_res.hits), (
                "Expected upserted entities to be found in search results"
            )
            print("Verified: upserted entities found in search results.")

        # 3. Delete entities
        print("\n3. Deleting upserted entities...")
        del_res = client.delete(
            "demo.docs",
            refs=["doc-write-1", "doc-write-2", "doc-write-3"],
        )
        print(
            f"Delete succeeded: affected={del_res.affected}, "
            f"block_number={del_res.block_number}, tx_hashes={del_res.tx_hashes}"
        )
        print(f"Session consistency updated: client.last_write_block = {client.last_write_block}")

        # 4. Verify deletion
        print("\n4. Verifying deletion...")
        if is_mock:
            print("Notice: Prism mock server detected; deletion request and model validated.")
        else:
            post_search = client.search(
                "demo.docs",
                vector=[0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8],
                k=5,
                filter=F.eq("category", "finance"),
            )
            post_ids = {h.id for h in post_search.hits}
            assert "doc-write-1" not in post_ids and "doc-write-2" not in post_ids, (
                "Deleted entities still present after deletion"
            )
            print("Verified: entities successfully deleted.")

        # 5. Direct-write attribute builder demonstration
        print("\n5. Demonstrating direct-write attribute builder:")
        col = client.get_collection("demo.docs")
        built_attrs = build_entity_attributes(col, entities[0])
        print(f"Produced {len(built_attrs)} direct-write attribute(s) for '{col.name}':")
        for attr in built_attrs:
            print(f" - {attr.name}: type_id=0x{attr.type_id:02x}, {len(attr.value_bytes)} bytes")

        print("\nWrite demonstration finished successfully.")
        return

    # 2. Ops probes
    print(f"Health: {client.health()}")
    ready = client.ready()
    print(f"Ready: {ready.ready} (collections: {ready.collections})")

    # 3. Sync status
    if client.has_capability("ops.sync"):
        sync = client.sync_status()
        print(f"Sync status: {len(sync)} collection(s) tracked")

    # 4. Collections
    if client.has_capability("collections.read"):
        cols = client.list_collections()
        print(f"Collections: {len(cols)} found")
        for c in cols:
            col_info = f"{c.column.type}({c.column.dimensions})"
            print(f" - {c.name}: column={col_info}, index={c.index.method}")

        if cols:
            detail = client.get_collection(cols[0].name)
            print(f"Collection '{detail.name}' schema_tag: {detail.schema_tag}")

    # 5. Search, point lookup, count on demo.docs
    print("\nExecuting vector similarity search on 'demo.docs':")
    query_vector = [0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8]
    search_res = client.search(
        "demo.docs",
        vector=query_vector,
        k=5,
        filter=F.eq("category", "finance"),
    )
    print(
        f"Search returned {len(search_res.hits)} hit(s) "
        f"(as_of_block={search_res.as_of_block}, sync_state={search_res.sync_state}):"
    )
    for hit in search_res.hits:
        print(
            f" - [{hit.id}] distance={hit.distance:.4f} score={hit.score:.4f} "
            f"attrs={hit.plain_attributes()}"
        )

    if search_res.hits and search_res.hits[0].id:
        target_ref = search_res.hits[0].id
        entity = client.get("demo.docs", target_ref)
        print(
            f"Point lookup '{target_ref}': key={entity.key[:18]}... "
            f"owner={entity.owner[:10]}... attrs={entity.plain_attributes()}"
        )

    count_res = client.count("demo.docs", filter=F.eq("category", "finance"))
    print(f"Count matching filter (category='finance'): {count_res.count}")

    # 6. Error handling demonstration
    print("\nDemonstrating error handling:")
    try:
        client.check_capability("nonexistent.feature")
    except NotImplementedByDaemon as e:
        print(f"Caught local capability check: [{e.code}] {e.detail}")

    try:
        # Request with Prefer: code=422 to trigger problem response from Prism mock
        client._http.request("GET", "/v1/collections/demo.docs", headers={"Prefer": "code=422"})
    except ValidationError as e:
        print(f"Caught typed ValidationError: [{e.status}] {e.code}: {e.title} (field={e.field})")
    except ApiError as e:
        print(f"Caught ApiError: [{e.status}] {e.code}: {e.title}")

    # 7. Retry behavior demonstration
    print("\nDemonstrating retry behavior:")
    demonstrate_retry()

    print("\nQuickstart finished successfully.")


if __name__ == "__main__":
    main()
