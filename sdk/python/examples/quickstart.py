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
    NotImplementedByDaemon,
    ValidationError,
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

    # 5. Error handling demonstration
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

    # 6. Retry behavior demonstration
    print("\nDemonstrating retry behavior:")
    demonstrate_retry()

    print("\nQuickstart finished successfully.")


if __name__ == "__main__":
    main()
