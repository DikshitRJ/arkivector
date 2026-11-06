#!/usr/bin/env python3
"""Seed mock-arkiv with the demo dataset entities."""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_FIXTURE = REPO_ROOT / "spec" / "fixtures" / "demo-dataset.json"


def http_post_json(url: str, payload: Any, timeout: float = 30.0) -> Any:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            content = resp.read()
            return json.loads(content) if content else None
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {e.code} from {url}: {err_body}") from e


def build_entity_payload(schema_tag: str, entity: dict[str, Any]) -> dict[str, Any]:
    vec_bytes = base64.b64decode(entity["vector_bytes"])
    doc_bytes = entity["document"].encode("utf-8")

    attrs: list[dict[str, Any]] = [
        {
            "name": "arkiv.schema",
            "type": "str",
            "value": schema_tag,
            "indexed": True,
        },
        {
            "name": "av.vec",
            "type": "bytes",
            "value": "0x" + vec_bytes.hex(),
            "indexed": False,
        },
        {
            "name": "av.id",
            "type": "str",
            "value": entity["id"],
            "indexed": True,
        },
        {
            "name": "av.doc",
            "type": "bytes",
            "value": "0x" + doc_bytes.hex(),
            "indexed": False,
        },
    ]

    for attr_name, attr_info in entity.get("attributes", {}).items():
        attr_type = attr_info["type"]
        attrs.append(
            {
                "name": attr_name,
                "type": attr_type,
                "value": attr_info["value"],
                "indexed": attr_type != "bytes",
            }
        )

    return {
        "key": entity["key"],
        "owner": entity.get("owner"),
        "creator": entity.get("creator"),
        "expires_at": entity.get("expires_at_block"),
        "attributes": attrs,
    }


def seed(rpc_url: str, fixture_path: Path) -> int:
    rpc_url = rpc_url.rstrip("/")
    with open(fixture_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    collection = data["collection"]
    schema_tag = collection["schema_tag"]
    entities = data["entities"]

    payloads = [build_entity_payload(schema_tag, e) for e in entities]

    # Post entities to /_mock/entities
    entities_url = f"{rpc_url}/_mock/entities"
    print(f"Inserting {len(payloads)} entities into {entities_url}...")
    http_post_json(entities_url, payloads, timeout=60.0)

    # Mine 1 block so entities are committed
    mine_url = f"{rpc_url}/_mock/mine"
    print(f"Mining 1 block at {mine_url}...")
    http_post_json(mine_url, {"n": 1})

    print(f"Successfully seeded {len(payloads)} entities.")
    return len(payloads)


def main() -> None:
    parser = argparse.ArgumentParser(description="Seed mock-arkiv with demo dataset")
    parser.add_argument(
        "--url",
        default=os.environ.get("ARKIV_RPC_URL", "http://127.0.0.1:8545"),
        help="mock-arkiv URL (default: http://127.0.0.1:8545 or $ARKIV_RPC_URL)",
    )
    parser.add_argument(
        "--fixture",
        type=Path,
        default=DEFAULT_FIXTURE,
        help="Path to demo dataset JSON fixture",
    )
    args = parser.parse_args()

    seed(args.url, args.fixture)


if __name__ == "__main__":
    main()
