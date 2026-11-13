"""Managed-write keys and fees (GitHub issues #13 and #14), against mock-arkiv.

#13  Entity keys follow the signer's on-chain entityNonce, not a per-request counter:
     - a wallet that already created entities elsewhere gets the right keys (wait receipt and none);
     - creates made elsewhere between two daemon requests are accounted for;
     - wait=none fails cleanly when entityNonce cannot be read; wait=receipt then takes keys from logs;
     - entities:prepare uses the wallet's entityNonce and pending transaction count.
#14  Fees come from eth_feeHistory, then eth_gasPrice, then the static config (with a cap);
     the gas limit is eth_estimateGas + gas_margin_pct, with a logged/counted fallback;
     a stuck (underpriced) transaction is replaced with the same nonce and bumped fees.
"""

from __future__ import annotations

import re
import sys
import time
from typing import Any

import pytest
from conftest import DEV_ADDR, MOCK_DIR, DaemonClient, DaemonFactory, MockArkiv

sys.path.insert(0, str(MOCK_DIR))
from raw_tx import decode_execute  # noqa: E402
from state import derive_key  # noqa: E402

CONTRACT = bytes.fromhex("44" + "00" * 18 + "44")
CHAIN_ID = 7794095


def make_collection(d: DaemonClient, name: str = "keys") -> None:
    status, body = d.post(
        "/v1/collections",
        {
            "name": name,
            "column": {"type": "vector", "dimensions": 3},
            "index": {"method": "none", "opclass": "vector_l2_ops"},
            "writers": [DEV_ADDR],
            "default_ttl": {"blocks": 100000},
        },
    )
    assert status == 201, body


def external_create(mock: MockArkiv, n: int = 1) -> None:
    """Entities created by the daemon's wallet outside the daemon (another tool, the Arkiv SDK)."""
    for i in range(n):
        mock.ctl("POST", "/entities", {"owner": DEV_ADDR, "attributes": [{"name": "elsewhere", "type": "u64", "value": i}]})


def chain_key_of(mock: MockArkiv, av_id: str) -> str:
    st = mock.ctl("GET", "/state")
    for e in st["entities"]:
        for a in e["attributes"]:
            if a["name"] == "av.id" and a["value"] == av_id:
                return e["key"].lower()
    raise AssertionError(f"no entity with av.id={av_id} on chain")


def upsert(d: DaemonClient, ids: list[str], wait: str) -> tuple[int, Any]:
    ents = [{"id": i, "vector": [1, 2, 3]} for i in ids]
    return d.post("/v1/collections/keys/entities", {"entities": ents, "wait": wait})


def metric(d: DaemonClient, name: str, labels: str = "") -> float:
    _, text, _ = d.http("GET", "/metrics")
    pat = re.escape(name) + (re.escape("{" + labels + "}") if labels else r"(?:\{\})?") + r" ([0-9.e+]+)"
    m = re.search(pat, text if isinstance(text, str) else "")
    return float(m.group(1)) if m else 0.0


@pytest.fixture
def writer(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> DaemonClient:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    d = daemon_factory.start()
    make_collection(d)
    return d


# --------------------------------------------------------------------------- issue #13


def test_keys_with_preexisting_entities(mock_arkiv: MockArkiv, writer: DaemonClient) -> None:
    external_create(mock_arkiv, 2)
    mock_arkiv.mine(1)

    status, body = upsert(writer, ["r1", "r2"], "receipt")
    assert status == 200, body
    for r in body["results"]:
        assert r["key"].lower() == chain_key_of(mock_arkiv, r["id"])

    # wait=none returns predicted keys: they must match what gets mined
    status, body = upsert(writer, ["n1", "n2", "n3"], "none")
    assert status == 202, body
    time.sleep(0.5)
    for r in body["results"]:
        assert r["key"].lower() == chain_key_of(mock_arkiv, r["id"])

    # creates elsewhere between two daemon requests (the local floor must not hide them)
    external_create(mock_arkiv, 3)
    mock_arkiv.mine(1)
    status, body = upsert(writer, ["n4"], "none")
    assert status == 202, body
    time.sleep(0.5)
    assert body["results"][0]["key"].lower() == chain_key_of(mock_arkiv, "n4")


def test_keys_when_entity_nonce_unreadable(mock_arkiv: MockArkiv, writer: DaemonClient) -> None:
    external_create(mock_arkiv, 1)
    mock_arkiv.mine(1)
    mock_arkiv.ctl("POST", "/faults", {"fail_methods": ["eth_call"]})

    # wait=none needs the prediction: clear error, nothing sent
    status, body = upsert(writer, ["x1"], "none")
    assert status == 502, body
    assert body["code"] == "arkiv_rpc_error"
    assert "entityNonce" in body["detail"]

    # wait=receipt takes the keys from the EntityCreated logs instead
    status, body = upsert(writer, ["x2", "x3"], "receipt")
    assert status == 200, body
    for r in body["results"]:
        assert r["key"].lower() == chain_key_of(mock_arkiv, r["id"])
    mock_arkiv.ctl("POST", "/faults", {"fail_methods": []})


def test_prepare_uses_wallet_nonces(mock_arkiv: MockArkiv, writer: DaemonClient) -> None:
    other = "0x" + "ab" * 20
    for i in range(3):
        mock_arkiv.ctl("POST", "/entities", {"owner": other, "attributes": [{"name": "n", "type": "u64", "value": i}]})
    mock_arkiv.mine(1)
    assert int(mock_arkiv.rpc("eth_getTransactionCount", [other, "pending"]), 16) == 3

    status, body = writer.post(
        "/v1/collections/keys/entities:prepare",
        {"from": other, "entities": [{"id": "p1", "vector": [1, 0, 0]}, {"id": "p2", "vector": [0, 1, 0]}]},
    )
    assert status == 200, body
    tx = body["transactions"][0]
    assert tx["nonce"] == 3
    ops = decode_execute(bytes.fromhex(tx["data"][2:]))
    creates = [o for o in ops if o.kind == "create"]
    owner = bytes.fromhex(other[2:])
    for i, (op, res) in enumerate(zip(creates, body["results"])):
        want = derive_key(CHAIN_ID, CONTRACT, owner, 3 + i, op.salt)
        assert res["key"].lower() == "0x" + want.hex()
    # gas estimate = eth_estimateGas (mock: 21000 + 16 * len + 50000) + 20%
    data_len = (len(tx["data"]) - 2) // 2
    est = 21000 + 16 * data_len + 50000
    assert tx["gas_estimate"] == est + est * 20 // 100


# --------------------------------------------------------------------------- issue #14


def test_fee_source_chain_and_cap(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    mock_arkiv.ctl("POST", "/fees", {"base_fee_wei": 2_000_000_000, "tip_wei": 300_000_000})
    d = daemon_factory.start(extra_toml="[write]\nmax_fee_gwei = 7\npriority_fee_gwei = 0.5\n")
    make_collection(d)

    # 1. eth_feeHistory: tip = percentile reward, max fee = 2 x base + tip
    status, body = upsert(d, ["f1"], "receipt")
    assert status == 200, body
    last = mock_arkiv.ctl("GET", "/fees")["last_tx"]
    assert last["tip"] == 300_000_000
    assert last["max_fee"] == 2 * 2_000_000_000 + 300_000_000
    est = 21000 + 16 * last["data_len"] + 50000
    assert last["gas"] == est + est * 20 // 100
    assert metric(d, "arkivector_write_fee_quotes_total", 'source="fee_history"') >= 1
    assert metric(d, "arkivector_write_gas_used_total") > 0

    # 2. no eth_feeHistory: eth_gasPrice (tip = min(static tip, gas price), max fee = 2 x gas price)
    mock_arkiv.ctl("POST", "/fees", {"fee_history": False})
    status, body = upsert(d, ["f2"], "receipt")
    assert status == 200, body
    last = mock_arkiv.ctl("GET", "/fees")["last_tx"]
    gas_price = 2_000_000_000 + 300_000_000
    assert last["tip"] == 500_000_000  # min(static 0.5 gwei, gas price 2.3 gwei)
    assert last["max_fee"] == 2 * gas_price
    assert metric(d, "arkivector_write_fee_quotes_total", 'source="gas_price"') >= 1

    # 3. neither: the static configuration
    mock_arkiv.ctl("POST", "/fees", {"gas_price": False})
    status, body = upsert(d, ["f3"], "receipt")
    assert status == 200, body
    last = mock_arkiv.ctl("GET", "/fees")["last_tx"]
    assert last["max_fee"] == 7_000_000_000
    assert last["tip"] == 500_000_000
    assert metric(d, "arkivector_write_fee_quotes_total", 'source="static"') >= 1


def test_fee_cap_and_gas_fallback(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    mock_arkiv.ctl("POST", "/fees", {"base_fee_wei": 5_000_000_000, "tip_wei": 1_000_000_000})
    d = daemon_factory.start(extra_toml="[write]\nmax_fee_cap_gwei = 3\ngas_margin_pct = 10\n")
    make_collection(d)

    status, body = upsert(d, ["c1"], "receipt")
    assert status == 200, body
    last = mock_arkiv.ctl("GET", "/fees")["last_tx"]
    assert last["max_fee"] == 3_000_000_000  # capped (uncapped would be 11 gwei)
    assert last["tip"] == 1_000_000_000
    est = 21000 + 16 * last["data_len"] + 50000
    assert last["gas"] == est + est * 10 // 100

    # eth_estimateGas down: fallback = base 60000 + 150000 per op, logged and counted
    mock_arkiv.ctl("POST", "/faults", {"fail_methods": ["eth_estimateGas"]})
    status, body = upsert(d, ["c2", "c3"], "receipt")
    assert status == 200, body
    last = mock_arkiv.ctl("GET", "/fees")["last_tx"]
    assert last["gas"] == 60000 + 2 * 150000
    assert metric(d, "arkivector_write_gas_estimate_fallback_total") >= 1
    mock_arkiv.ctl("POST", "/faults", {"fail_methods": []})


def test_stuck_transaction_is_replaced(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    # quoted tip 0.1 gwei; the node only mines tips >= 0.13 gwei: three +12.5% bumps get there
    mock_arkiv.ctl("POST", "/fees", {"tip_wei": 100_000_000, "min_tip_wei": 130_000_000})
    d = daemon_factory.start(extra_toml="[write]\nreplace_after_blocks = 2\nmax_replacements = 3\n")
    make_collection(d)

    status, body = upsert(d, ["s1"], "receipt")
    assert status == 200, body
    fees = mock_arkiv.ctl("GET", "/fees")
    assert fees["replaced_total"] == 3
    assert fees["held"] == 0
    assert fees["last_tx"]["tip"] >= 130_000_000
    # the reported hash is the replacement that was mined, and its receipt exists
    assert body["tx_hashes"][0] == fees["last_tx"]["hash"]
    assert mock_arkiv.rpc("eth_getTransactionReceipt", [body["tx_hashes"][0]])["status"] == "0x1"
    assert body["results"][0]["key"].lower() == chain_key_of(mock_arkiv, "s1")
    assert metric(d, "arkivector_write_tx_replacements_total") == 3


def test_stuck_transaction_gives_up(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    mock_arkiv.ctl("POST", "/fees", {"tip_wei": 100_000_000, "min_tip_wei": 10_000_000_000})
    d = daemon_factory.start(extra_toml="[write]\nreplace_after_blocks = 1\nmax_replacements = 1\n")
    make_collection(d)
    status, body = d.post(
        "/v1/collections/keys/entities", {"entities": [{"id": "g1", "vector": [1, 2, 3]}], "wait": "receipt", "timeout_ms": 3000}
    )
    assert status == 504, body
    assert body["code"] == "consistency_timeout"
    assert mock_arkiv.ctl("GET", "/fees")["replaced_total"] == 1
    # The next request resyncs the transaction nonce (no permanent gap): it reuses the stuck nonce, so its
    # fees must beat the held transaction by 10% like on a real node.
    mock_arkiv.ctl("POST", "/fees", {"min_tip_wei": 0, "tip_wei": 1_000_000_000})
    status, body = upsert(d, ["g2"], "receipt")
    assert status == 200, body
