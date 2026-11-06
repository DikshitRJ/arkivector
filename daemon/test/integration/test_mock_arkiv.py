"""Conformance tests for daemon/testkit/mock-arkiv against the Arkiv docs (RESEARCH/arkiv-docs).

Run: pytest -q daemon/test/integration -m mock
"""

from __future__ import annotations

import json
from typing import Any

import pytest
from conftest import CONTRACT, DEFAULT_OWNER, HttpStatusError, JsonRpcError, MockArkiv
from websockets.sync.client import connect

pytestmark = pytest.mark.mock

TOPICS = {
    "created": "0xb282d7c494b8899aa8015cd07be621530beb03409eb8c5e8fdc1411ba64356a5",
    "patched": "0xe986ee220b2d7947a009d9389d8400b2ac82d3b430a9f72700a0975d3d5a34fe",
    "extended": "0x10dc3526654c9ba4e2370b1bfcbc0de087d141a5694b3d3305d37df7e6705d4b",
    "transferred": "0x0b659dccc8eb950324170e8d9598af5ee04ee070883eb28651a96788721fbf83",
    "deleted": "0x4059b76c47e1ecc40ba88e649b654f716515eb358c3ce83c803820e3b3130cc3",
}
OWNER_A = "0x" + "aa" * 20
OWNER_B = "0x" + "bb" * 20
OWNER_C = "0x" + "cc" * 20


def attr(name: str, typ: str, value: Any, **kw: Any) -> dict[str, Any]:
    return {"name": name, "type": typ, "value": value, **kw}


def count(m: MockArkiv, q: str) -> int:
    return int(m.query(q, select={"count": True})["count"], 16)


def err_code(m: MockArkiv, method: str, params: Any) -> int:
    with pytest.raises(JsonRpcError) as ei:
        m.rpc(method, params)
    return ei.value.code


# ---------------------------------------------------------------------------
# Basics
# ---------------------------------------------------------------------------
def test_chain_basics(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    assert m.rpc("eth_chainId") == "0x76edaf"
    assert m.head() == 0
    t = m.rpc("arkiv_getBlockTiming")
    assert t["current_block"] == 0 and t["duration"] > 0
    m.mine(3)
    assert m.head() == 3
    blk = m.rpc("eth_getBlockByNumber", ["0x3", False])
    assert blk["number"] == "0x3"
    assert blk["parentHash"] == m.rpc("eth_getBlockByNumber", ["0x2", False])["hash"]
    assert m.rpc("eth_getBlockByNumber", ["0x99", False]) is None


def test_unknown_method_and_bad_params(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    assert err_code(m, "no_such_method", []) == -32601
    assert err_code(m, "arkiv_query", "not-a-list") == -32602
    assert err_code(m, "arkiv_query", []) == -32602
    assert err_code(m, "arkiv_getEntity", ["0x1234"]) == -32602
    assert err_code(m, "arkiv_query", ["n = 1", {"bogus": 1}]) == -32602
    assert err_code(m, "arkiv_query", ["n = 1", {"select": {"bogus": True}}]) == -32602


def test_pending_txs_invisible_until_mined(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(min_lifetime=100)
    assert m.rpc("arkiv_getEntity", [k]) is None
    assert m.ctl("GET", "/state")["pending"]["txs"] == 1
    m.mine()
    assert m.rpc("arkiv_getEntity", [k])["key"] == k


# ---------------------------------------------------------------------------
# Query limits, pagination
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("bad", ["0x65", "0x0", "0x3e8", 100, "100", "0xzz", None])
def test_limit_out_of_range_rejected(mock_arkiv: MockArkiv, bad: Any) -> None:
    assert err_code(mock_arkiv, "arkiv_query", ["$owner != addr(" + DEFAULT_OWNER + ")", {"limit": bad}]) == -32602


@pytest.mark.parametrize("ok", ["0x1", "0x64"])
def test_limit_boundaries_accepted(mock_arkiv: MockArkiv, ok: str) -> None:
    m = mock_arkiv
    m.ctl("POST", "/entities", [{"min_lifetime": 100} for _ in range(120)])
    m.mine()
    res = m.query("$owner = addr(" + DEFAULT_OWNER + ")", limit=ok)
    assert len(res["data"]) == int(ok, 16)


def test_offset_validation_and_paging(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    m.ctl("POST", "/entities", [{"min_lifetime": 100} for _ in range(5)])
    m.mine()
    q = "$owner = addr(" + DEFAULT_OWNER + ") ORDER BY $key ASC"
    allk = m.keys(q)
    assert m.keys(q, limit="0x2", offset="0x3") == allk[3:5]
    assert m.keys(q, offset="0x99") == []
    assert err_code(m, "arkiv_query", [q, {"offset": "-1"}]) == -32602


def test_cursor_pagination_300_entities_each_exactly_once(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    items = [{"min_lifetime": 1000, "attributes": [attr("tag", "str", "page"), attr("n", "i32", i)]} for i in range(300)]
    created = {x["keys"][0] for x in m.ctl("POST", "/entities", items)["created"]}
    assert len(created) == 300
    m.mine()
    seen: list[str] = []
    cursor = None
    pages = 0
    while True:
        q = "tag = str('page')" + (f" AND $key > key({cursor})" if cursor else "") + " ORDER BY $key ASC"
        page = m.keys(q, select={"key": True}, limit="0x64")
        pages += 1
        seen += page
        if len(page) < 100:
            break
        cursor = page[-1]
    assert pages == 4  # 100 + 100 + 100 + 0
    assert len(seen) == 300 and set(seen) == created and seen == sorted(seen)
    # default order (no ORDER BY) is creation order
    assert len(m.keys("tag = str('page')")) == 100


# ---------------------------------------------------------------------------
# Liveness
# ---------------------------------------------------------------------------
def test_expiry_visible_at_e_minus_1_gone_at_e(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(min_lifetime=3, attributes=[attr("t", "str", "ttl")])
    m.mine()  # creation block 1, expiry = 1 + 3
    e = int(m.rpc("arkiv_getEntity", [k])["expiresAt"], 16)
    assert e == 4
    m.mine(e - 1 - m.head())  # head == e-1
    assert m.head() == e - 1
    assert count(m, "t = str('ttl')") == 1 and m.rpc("arkiv_getEntity", [k]) is not None
    assert count(m, f"$expiresAt = u64({e})") == 1
    m.mine()  # head == e
    assert m.head() == e
    assert count(m, "t = str('ttl')") == 0 and m.rpc("arkiv_getEntity", [k]) is None
    assert count(m, f"$expiresAt = u64({e})") == 0


def test_extend_keeps_entity_alive_past_old_expiry(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(min_lifetime=2)
    m.mine()
    m.ctl("POST", f"/entities/{k}/extend", {"expires_at": 50})
    m.mine(5)
    assert m.rpc("arkiv_getEntity", [k]) is not None


def test_op_on_expired_entity_reverts(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(min_lifetime=1)
    m.mine(3)
    status, body = m.ctl_status("DELETE", f"/entities/{k}")
    assert status == 409 and body["error"] == "EntityExpired"


def test_not_owner_reverts_and_invalid_input(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(owner=OWNER_A, min_lifetime=100)
    m.mine()
    status, body = m.ctl_status("DELETE", f"/entities/{k}", {"owner": OWNER_B})
    assert status == 409 and body["error"] == "NotOwner"
    assert m.ctl_status("POST", "/entities", {"attributes": "nope"})[0] == 400
    assert m.rpc("arkiv_getEntity", [k]) is not None


# ---------------------------------------------------------------------------
# Ordering
# ---------------------------------------------------------------------------
def test_order_by_missing_attribute_placement_and_key_tiebreak(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    spec: list[tuple[str, list[dict[str, Any]]]] = [
        ("lo", [attr("grp", "str", "g"), attr("score", "dec", "1")]),
        ("hi", [attr("grp", "str", "g"), attr("score", "dec", "5")]),
        ("tie1", [attr("grp", "str", "g"), attr("score", "dec", "3")]),
        ("tie2", [attr("grp", "str", "g"), attr("score", "dec", "3")]),
        ("miss1", [attr("grp", "str", "g")]),
        ("miss2", [attr("grp", "str", "g")]),
        ("mismatch", [attr("grp", "str", "g"), attr("score", "str", "oops")]),  # type mismatch == missing
    ]
    ids = {name: m.create(min_lifetime=100, attributes=attrs, salt=i) for i, (name, attrs) in enumerate(spec)}
    m.mine()
    inv = {v: k for k, v in ids.items()}
    keyord = lambda *names: sorted(ids[n] for n in names)  # noqa: E731

    asc = m.keys("grp = str('g') ORDER BY score:dec ASC")
    missing = keyord("miss1", "miss2", "mismatch")
    assert asc[:3] == missing, [inv[k] for k in asc]  # first in ASC, $key tie-break
    assert asc[3] == ids["lo"]
    assert asc[4:6] == keyord("tie1", "tie2")
    assert asc[6] == ids["hi"]

    desc = m.keys("grp = str('g') ORDER BY score:dec DESC")
    assert desc[0] == ids["hi"]
    assert desc[1:3] == keyord("tie1", "tie2")  # ties still $key ascending
    assert desc[3] == ids["lo"]
    assert desc[4:] == missing  # last in DESC


def test_order_by_system_fields(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    for i, own in enumerate((OWNER_C, OWNER_A, OWNER_B)):
        m.create(owner=own, min_lifetime=10 + i)
    m.mine()
    owners = [e["owner"].lower() for e in m.query("$expiresAt > u64(0) ORDER BY $owner ASC", select={"owner": True})["data"]]
    assert owners == [OWNER_A, OWNER_B, OWNER_C]
    exp = [int(e["expiresAt"], 16) for e in m.query("$expiresAt > u64(0) ORDER BY $expiresAt DESC", select={"expiresAt": True})["data"]]
    assert exp == sorted(exp, reverse=True)
    # type suffix on a system field must match its intrinsic type
    assert err_code(m, "arkiv_query", ["$expiresAt > u64(0) ORDER BY $expiresAt:i32 ASC"]) == -32002
    # bytes is not sortable; attribute order needs a type suffix
    assert err_code(m, "arkiv_query", ["x = 1 ORDER BY x:bytes"]) == -32002
    assert err_code(m, "arkiv_query", ["x = 1 ORDER BY x"]) == -32001


# ---------------------------------------------------------------------------
# Logs
# ---------------------------------------------------------------------------
def _five_op_block(m: MockArkiv) -> tuple[str, int]:
    k = m.create(owner=OWNER_B, min_lifetime=100)
    m.ctl("PATCH", f"/entities/{k}", {"attributes": [attr("a", "bool", True)]})
    m.ctl("POST", f"/entities/{k}/extend", {"min_lifetime": 200})
    m.ctl("POST", f"/entities/{k}/transfer", {"new_owner": OWNER_C})
    m.ctl("DELETE", f"/entities/{k}")
    m.mine()
    return k, m.head()


def test_logs_topics_and_ordering_for_all_five_ops(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k, h = _five_op_block(m)
    logs = m.rpc("eth_getLogs", [{"fromBlock": hex(h), "toBlock": hex(h), "address": CONTRACT}])
    assert [lg["topics"][0] for lg in logs] == [TOPICS[x] for x in ("created", "patched", "extended", "transferred", "deleted")]
    assert [int(lg["logIndex"], 16) for lg in logs] == [0, 1, 2, 3, 4]
    assert all(lg["address"].lower() == CONTRACT for lg in logs)
    assert all(lg["blockNumber"] == hex(h) and lg["removed"] is False for lg in logs)
    assert all(lg["topics"][1] == k for lg in logs)  # entityKey is topic1 for all five
    assert logs[0]["topics"][2].endswith("bb" * 20)  # created: owner indexed
    assert logs[3]["topics"][2].endswith("bb" * 20) and logs[3]["topics"][3].endswith("cc" * 20)  # old, new owner
    # five separate txs -> five distinct tx hashes, transactionIndex 0..4
    assert [int(lg["transactionIndex"], 16) for lg in logs] == [0, 1, 2, 3, 4]
    assert len({lg["transactionHash"] for lg in logs}) == 5
    rcpt = m.rpc("eth_getTransactionReceipt", [logs[0]["transactionHash"]])
    assert rcpt["status"] == "0x1" and rcpt["logs"][0]["topics"][0] == TOPICS["created"]


def test_multi_op_tx_one_hash_and_calldata_selector(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    r = m.ctl(
        "POST",
        "/tx",
        {"owner": OWNER_A, "ops": [{"op": "create", "min_lifetime": 9}, {"op": "create", "min_lifetime": 9, "salt": 1}]},
    )
    m.mine()
    logs = m.rpc("eth_getLogs", [{"fromBlock": "0x1", "toBlock": "0x1"}])
    assert len(logs) == 2
    assert logs[0]["transactionHash"] == logs[1]["transactionHash"] == r["txHash"]
    assert [int(x["logIndex"], 16) for x in logs] == [0, 1]
    tx = m.rpc("eth_getTransactionByHash", [r["txHash"]])
    assert tx["input"].startswith("0x49650044")
    assert m.rpc("eth_getTransactionByHash", ["0x" + "00" * 32]) is None


def test_multi_op_tx_is_atomic(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(owner=OWNER_A, min_lifetime=100)
    m.mine()
    status, body = m.ctl_status(
        "POST",
        "/tx",
        {"owner": OWNER_B, "ops": [{"op": "create", "min_lifetime": 9, "salt": 5}, {"op": "delete", "key": k}]},
    )
    assert status == 409 and body["error"] == "NotOwner"
    m.mine()
    assert m.rpc("eth_getLogs", [{"fromBlock": "0x2", "toBlock": "0x2"}]) == []
    assert m.rpc("arkiv_getEntity", [k]) is not None


def test_getlogs_address_and_topic_filtering(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k, h = _five_op_block(m)
    rng = {"fromBlock": hex(h), "toBlock": hex(h)}
    other = "0x" + "55" * 20
    assert m.rpc("eth_getLogs", [{**rng, "address": other}]) == []
    assert len(m.rpc("eth_getLogs", [{**rng, "address": [other, CONTRACT]}])) == 5
    assert len(m.rpc("eth_getLogs", [rng])) == 5
    one = m.rpc("eth_getLogs", [{**rng, "topics": [TOPICS["deleted"]]}])
    assert len(one) == 1 and one[0]["topics"][1] == k
    two = m.rpc("eth_getLogs", [{**rng, "topics": [[TOPICS["created"], TOPICS["deleted"]]]}])
    assert [x["topics"][0] for x in two] == [TOPICS["created"], TOPICS["deleted"]]
    assert len(m.rpc("eth_getLogs", [{**rng, "topics": [None, k]}])) == 5  # null wildcard + entityKey
    assert m.rpc("eth_getLogs", [{**rng, "topics": [None, "0x" + "12" * 32]}]) == []
    assert len(m.rpc("eth_getLogs", [{**rng, "topics": [TOPICS["transferred"], k, "0x" + "00" * 12 + "bb" * 20]}])) == 1
    assert m.rpc("eth_getLogs", [{**rng, "topics": [TOPICS["transferred"], k, "0x" + "00" * 12 + "dd" * 20]}]) == []
    by_hash = m.rpc("eth_getBlockByNumber", [hex(h), False])["hash"]
    assert len(m.rpc("eth_getLogs", [{"blockHash": by_hash}])) == 5
    # malformed filter
    assert err_code(m, "eth_getLogs", [{"address": "0x1234"}]) == -32602
    assert err_code(m, "eth_getLogs", []) == -32602


def test_getlogs_range_cap(mock_arkiv_factory: Any) -> None:
    m: MockArkiv = mock_arkiv_factory("--max-log-range", "5")
    m.mine(20)
    # exactly 5 blocks is allowed, 6 is not
    assert m.rpc("eth_getLogs", [{"fromBlock": "0x1", "toBlock": "0x5"}]) == []
    with pytest.raises(JsonRpcError) as ei:
        m.rpc("eth_getLogs", [{"fromBlock": "0x1", "toBlock": "0x6"}])
    assert ei.value.code == -32005
    assert ei.value.data == {"maxRange": 5, "requested": 6}
    with pytest.raises(JsonRpcError) as ei:
        m.rpc("eth_getLogs", [{"fromBlock": "0x0", "toBlock": "latest"}])
    assert ei.value.code == -32005
    # inverted range is not an error and returns nothing
    assert m.rpc("eth_getLogs", [{"fromBlock": "0x9", "toBlock": "0x3"}]) == []


# ---------------------------------------------------------------------------
# Reorg
# ---------------------------------------------------------------------------
def test_reorg_replaces_blocks_and_entity_state_follows(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    m.mine(1)
    k_old = m.create(owner=OWNER_A, min_lifetime=100, attributes=[attr("side", "str", "old")])
    m.mine()  # block 2
    k_old2 = m.create(owner=OWNER_A, min_lifetime=100, attributes=[attr("side", "str", "old")], salt=1)
    m.mine()  # block 3
    before = {n: m.rpc("eth_getBlockByNumber", [hex(n), False])["hash"] for n in range(0, 4)}

    r = m.ctl(
        "POST",
        "/reorg",
        {"depth": 2, "replacement_ops": [{"op": "create", "owner": OWNER_B, "min_lifetime": 100, "attributes": [attr("side", "str", "new")]}]},
    )
    assert r["head"] == 3 and len(r["removed"]) == 2 and len(r["added"]) == 2
    assert r["old_hashes"] == [before[2], before[3]]
    after = {n: m.rpc("eth_getBlockByNumber", [hex(n), False])["hash"] for n in range(0, 4)}
    assert after[0] == before[0] and after[1] == before[1]  # common ancestor untouched
    assert after[2] != before[2] and after[3] != before[3]
    parent = m.rpc("eth_getBlockByNumber", ["0x3", False])["parentHash"]
    assert parent == after[2]  # chain still linked
    assert m.head() == 3
    # entity state follows the replacement ops
    assert m.rpc("arkiv_getEntity", [k_old]) is None and m.rpc("arkiv_getEntity", [k_old2]) is None
    assert count(m, "side = str('old')") == 0
    new = m.query("side = str('new')", select={"owner": True})["data"]
    assert len(new) == 1 and new[0]["owner"].lower() == OWNER_B
    # logs follow: the replaced block carries only the new Created log
    logs = m.rpc("eth_getLogs", [{"fromBlock": "0x2", "toBlock": "0x3"}])
    assert len(logs) == 1 and logs[0]["blockNumber"] == "0x2" and logs[0]["topics"][1] == new[0]["key"]
    assert m.rpc("eth_getBlockByHash", [before[2], False]) is None  # orphaned block gone


def test_reorg_extra_blocks_and_too_deep_fails(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    m.mine(3)
    r = m.ctl("POST", "/reorg", {"depth": 1, "extra_blocks": 2})
    assert r["head"] == 5 and len(r["added"]) == 3
    status, _ = m.ctl_status("POST", "/reorg", {"depth": 50})
    assert status >= 400
    assert m.head() == 5
    assert m.ctl_status("POST", "/reorg", {"depth": 0})[0] == 400


def test_reorg_failing_replacement_leaves_chain_untouched(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(owner=OWNER_A, min_lifetime=100)
    m.mine(2)
    hashes = [m.rpc("eth_getBlockByNumber", [hex(n), False])["hash"] for n in range(3)]
    status, body = m.ctl_status(
        "POST", "/reorg", {"depth": 2, "replacement_ops": [{"op": "delete", "key": "0x" + "99" * 32, "owner": OWNER_B}]}
    )
    assert status >= 400, body
    assert [m.rpc("eth_getBlockByNumber", [hex(n), False])["hash"] for n in range(3)] == hashes
    assert m.rpc("arkiv_getEntity", [k]) is not None


def test_reorg_discards_pending_txs(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    m.mine(2)
    m.create(min_lifetime=100)
    assert m.ctl("GET", "/state")["pending"]["txs"] == 1
    m.ctl("POST", "/reorg", {"depth": 1})
    assert m.ctl("GET", "/state")["pending"]["txs"] == 0


# ---------------------------------------------------------------------------
# Faults
# ---------------------------------------------------------------------------
def test_http_429_on_rpc_not_on_control(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    m.ctl("POST", "/faults", {"http_429_ratio": 1})
    with pytest.raises(HttpStatusError) as ei:
        m.rpc("eth_blockNumber")
    assert ei.value.status == 429 and ei.value.headers.get("Retry-After") == "1"
    assert m.ctl_status("GET", "/state")[0] == 200
    assert m.ctl_status("POST", "/mine", {"n": 1})[0] == 200
    assert m.ctl("GET", "/faults")["http_429_ratio"] == 1.0
    m.ctl("POST", "/faults", {"http_429_ratio": 0})
    assert m.rpc("eth_blockNumber") == "0x1"


def test_fail_methods_and_validation(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    m.ctl("POST", "/faults", {"fail_methods": ["arkiv_query"]})
    assert err_code(m, "arkiv_query", ["n = 1"]) == -32603
    assert m.rpc("eth_blockNumber") == "0x0"  # other methods unaffected
    m.ctl("POST", "/faults", {"fail_methods": {"*": 1}})
    assert err_code(m, "eth_blockNumber", []) == -32603
    m.ctl("POST", "/faults", {"fail_methods": []})
    assert m.rpc("eth_blockNumber") == "0x0"
    # invalid fault bodies are rejected (HTTP 400), state unchanged
    for bad in ({"http_429_ratio": 2}, {"latency_ms": -1}, {"nope": 1}, {"fail_methods": "x"}):
        assert m.ctl_status("POST", "/faults", bad)[0] == 400
    assert m.ctl("GET", "/faults")["http_429_ratio"] == 0.0


def test_partial_ratio_is_seeded_and_reset_clears_faults(mock_arkiv_factory: Any) -> None:
    def run() -> list[int]:
        m: MockArkiv = mock_arkiv_factory()
        m.ctl("POST", "/faults", {"http_429_ratio": 0.5})
        out = [m.rpc_raw("eth_blockNumber")[0] for _ in range(30)]
        m.ctl("POST", "/reset", {"seed": 7})
        assert m.ctl("GET", "/faults")["http_429_ratio"] == 0.0
        return out

    a, b = run(), run()
    assert a == b and 429 in a and 200 in a


def test_batch_and_notification(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    status, out, _ = m.http(
        "POST", "/", [{"jsonrpc": "2.0", "id": 1, "method": "eth_chainId"}, {"jsonrpc": "2.0", "id": 2, "method": "nope"}]
    )
    assert status == 200 and out[0]["result"] == "0x76edaf" and out[1]["error"]["code"] == -32601
    status, out, _ = m.http("POST", "/", [])
    assert out["error"]["code"] == -32600
    assert m.http("POST", "/", {"jsonrpc": "2.0", "method": "eth_chainId"})[0] == 204
    # any access key path is accepted
    assert m.http("POST", "/some-key", {"jsonrpc": "2.0", "id": 1, "method": "eth_chainId"})[1]["result"] == "0x76edaf"


# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------
def test_ws_newheads_one_notification_per_block(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    with connect(m.ws_url, open_timeout=10) as ws:
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_subscribe", "params": ["newHeads"]}))
        sub = json.loads(ws.recv(timeout=10))
        sid = sub["result"]
        assert sid.startswith("0x")
        m.mine(3)
        got = [json.loads(ws.recv(timeout=10)) for _ in range(3)]
        assert [g["params"]["result"]["number"] for g in got] == ["0x1", "0x2", "0x3"]
        assert all(g["method"] == "eth_subscription" and g["params"]["subscription"] == sid for g in got)
        assert got[1]["params"]["result"]["parentHash"] == got[0]["params"]["result"]["hash"]
        # unsubscribe stops notifications
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "eth_unsubscribe", "params": [sid]}))
        assert json.loads(ws.recv(timeout=10))["result"] is True
        m.mine(1)
        with pytest.raises(TimeoutError):
            ws.recv(timeout=0.5)
        # regular RPC works over the socket
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 3, "method": "eth_blockNumber", "params": []}))
        assert json.loads(ws.recv(timeout=10))["result"] == "0x4"


def test_ws_logs_subscription_filter_and_removed_on_reorg(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    with connect(m.ws_url, open_timeout=10) as ws:
        flt = {"address": CONTRACT, "topics": [TOPICS["created"]]}
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_subscribe", "params": ["logs", flt]}))
        sid = json.loads(ws.recv(timeout=10))["result"]
        k = m.create(owner=OWNER_A, min_lifetime=100)
        m.ctl("POST", f"/entities/{k}/extend", {"min_lifetime": 200})  # filtered out
        m.mine()
        msg = json.loads(ws.recv(timeout=10))
        assert msg["params"]["subscription"] == sid and msg["params"]["result"]["topics"][1] == k
        assert msg["params"]["result"]["removed"] is False
        with pytest.raises(TimeoutError):
            ws.recv(timeout=0.5)
        m.ctl("POST", "/reorg", {"depth": 1})
        msg = json.loads(ws.recv(timeout=10))
        assert msg["params"]["result"]["removed"] is True and msg["params"]["result"]["topics"][1] == k


def test_ws_subscribe_errors_and_http_rejects_subscribe(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    assert err_code(m, "eth_subscribe", ["newHeads"]) == -32601  # needs a websocket
    with connect(m.ws_url, open_timeout=10) as ws:
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_subscribe", "params": ["bogus"]}))
        assert json.loads(ws.recv(timeout=10))["error"]["code"] == -32602
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "eth_subscribe", "params": []}))
        assert json.loads(ws.recv(timeout=10))["error"]["code"] == -32602


def test_ws_drop_fault_closes_socket(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    with connect(m.ws_url, open_timeout=10) as ws:
        ws.send(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_subscribe", "params": ["newHeads"]}))
        ws.recv(timeout=10)
        m.ctl("POST", "/faults", {"drop_ws_ratio": 1})
        m.mine()
        with pytest.raises(Exception) as ei:  # ConnectionClosed
            ws.recv(timeout=5)
            ws.recv(timeout=5)
        assert "ConnectionClosed" in type(ei.value).__mro__[0].__name__ or "close" in str(ei.value).lower() or "1000" in str(ei.value)


# ---------------------------------------------------------------------------
# Determinism
# ---------------------------------------------------------------------------
def _scenario(m: MockArkiv) -> tuple[list[str], list[str]]:
    keys = [
        m.create(owner=OWNER_A, min_lifetime=100, salt=s, attributes=[attr("i", "i32", s)]) for s in range(4)
    ]
    m.mine(2)
    m.ctl("DELETE", f"/entities/{keys[0]}")
    m.ctl("POST", f"/entities/{keys[1]}/transfer", {"new_owner": OWNER_B})
    m.mine()
    m.ctl("POST", "/reorg", {"depth": 2, "replacement_ops": [{"op": "create", "owner": OWNER_C, "min_lifetime": 50}]})
    hashes = [m.rpc("eth_getBlockByNumber", [hex(n), False])["hash"] for n in range(m.head() + 1)]
    return hashes, keys + m.keys("$expiresAt > u64(0) ORDER BY $key ASC")


def test_determinism_same_seed_same_chain(mock_arkiv_factory: Any) -> None:
    a: MockArkiv = mock_arkiv_factory(seed=7)
    b: MockArkiv = mock_arkiv_factory(seed=7)
    ha, ka = _scenario(a)
    hb, kb = _scenario(b)
    assert ha == hb and ka == kb
    assert len(set(ha)) == len(ha)
    # tx hashes and block timestamps too
    assert a.rpc("eth_getBlockByNumber", ["0x1", False])["timestamp"] == b.rpc("eth_getBlockByNumber", ["0x1", False])["timestamp"]
    assert a.rpc("eth_getLogs", [{"fromBlock": "0x1", "toBlock": "0x5"}]) == b.rpc("eth_getLogs", [{"fromBlock": "0x1", "toBlock": "0x5"}])


def test_determinism_different_seed_differs_and_reset_restores(mock_arkiv_factory: Any) -> None:
    a: MockArkiv = mock_arkiv_factory(seed=7)
    b: MockArkiv = mock_arkiv_factory(seed=8)
    ha, _ = _scenario(a)
    hb, _ = _scenario(b)
    assert ha[1:] != hb[1:] or ha != hb
    a.ctl("POST", "/reset", {"seed": 7})
    assert a.head() == 0
    c: MockArkiv = mock_arkiv_factory(seed=7)
    assert _scenario(a) == _scenario(c)


def test_reorg_changes_hashes_deterministically(mock_arkiv_factory: Any) -> None:
    outs = []
    for _ in range(2):
        m: MockArkiv = mock_arkiv_factory(seed=3)
        m.mine(3)
        before = m.rpc("eth_getBlockByNumber", ["0x3", False])["hash"]
        r = m.ctl("POST", "/reorg", {"depth": 1})
        outs.append((before, r["added"][0]["hash"]))
        assert before != r["added"][0]["hash"]
    assert outs[0] == outs[1]


# ---------------------------------------------------------------------------
# Query language
# ---------------------------------------------------------------------------
@pytest.fixture
def qdata(mock_arkiv: MockArkiv) -> MockArkiv:
    m = mock_arkiv
    rows = [
        ("a", OWNER_A, 1, "1.5", "alpha", True, [1]),
        ("b", OWNER_A, 5, "2.5", "alpine", False, [2]),
        ("c", OWNER_B, 10, "-3", "beta", True, [3]),
        ("d", OWNER_B, 20, "100", "ALPHA", False, [4]),
    ]
    for i, (_, owner, n, d, s, b, raw) in enumerate(rows):
        m.create(
            owner=owner,
            salt=i,
            min_lifetime=100,
            attributes=[
                attr("id", "str", rows[i][0]),
                attr("n", "i32", n),
                attr("big", "u256", 2**200 + n),
                attr("u", "u64", 2**40 + n),
                attr("d", "dec", d),
                attr("s", "str", s),
                attr("b", "bool", b),
                attr("blob", "bytes", "0x" + bytes(raw).hex()),
                attr("hidden", "str", "x", indexed=False),
            ],
        )
    m.create(owner=OWNER_C, salt=9, min_lifetime=100, attributes=[attr("id", "str", "e"), attr("only_e", "str", "z")])
    m.mine()
    return m


def ids(m: MockArkiv, q: str) -> list[str]:
    out = m.query(q + " ORDER BY id:str ASC" if "ORDER BY" not in q else q, select={"attributes": {"id": True}})["data"]
    return [e["attributes"][0]["value"] for e in out]


@pytest.mark.parametrize(
    "q,expected",
    [
        ("n = 5", ["b"]),
        ("n = i32(5)", ["b"]),
        ("n != 5 AND EXISTS(n)", ["a", "c", "d"]),
        ("n < 5", ["a"]),
        ("n <= 5", ["a", "b"]),
        ("n > 10", ["d"]),
        ("n >= 10", ["c", "d"]),
        ("n BETWEEN 5 AND 10", ["b", "c"]),
        ("n BETWEEN 10 AND 5", []),
        ("d BETWEEN dec(1) AND dec('2.5')", ["a", "b"]),
        ("d < dec(0)", ["c"]),
        ("d = dec(1.5)", ["a"]),
        ("big = u256(" + str(2**200 + 10) + ")", ["c"]),
        ("big > u256(" + str(2**200 + 5) + ")", ["c", "d"]),
        ("u >= u64(" + str(2**40 + 10) + ")", ["c", "d"]),
        ("b = true", ["a", "c"]),
        ("b = bool(false)", ["b", "d"]),
        ("s = str('beta')", ["c"]),
        ("s STARTSWITH str('al')", ["a", "b"]),  # case sensitive: 'ALPHA' excluded
        ("s STARTSWITH str('')", ["a", "b", "c", "d"]),
        ("s STARTSWITH str('alphabet')", []),
        ("EXISTS(n)", ["a", "b", "c", "d"]),
        ("EXISTS(only_e)", ["e"]),
        ("NOT EXISTS(n)", ["e"]),
        ("EXISTS(hidden)", []),  # unindexed attributes never match
        ("hidden = str('x')", []),
        ("EXISTS(nonexistent)", []),
        ("TYPEOF(n) = i32", ["a", "b", "c", "d"]),
        ("TYPEOF(n) = str", []),
        ("TYPEOF(id) = str", ["a", "b", "c", "d", "e"]),
        ("TYPEOF(blob) = bytes", []),  # bytes are unindexed
        ("n = 1 OR n = 20", ["a", "d"]),
        ("n = 1 AND s = str('alpha')", ["a"]),
        ("n = 1 AND s = str('beta')", []),
        ("NOT n = 1 AND EXISTS(n)", ["b", "c", "d"]),
        ("NOT (n = 1 OR n = 5) AND EXISTS(n)", ["c", "d"]),
        ("(n = 1 OR n = 5) AND b = true", ["a"]),
        ("n = 1 OR n = 5 AND b = true", ["a"]),  # AND binds tighter than OR
        ("((n = 1))", ["a"]),
        ("n = 1 and s = str('alpha')", ["a"]),  # keywords are case-insensitive
        ("-- leading comment\nn = 5", ["b"]),
        ("n = 5 -- trailing comment", ["b"]),
        ("n = 5 -- c1\n OR n = 1 -- c2", ["a", "b"]),
        ("n = -3 OR n = 1", ["a"]),
        (f"$owner = addr({OWNER_A})", ["a", "b"]),
        (f"$owner != addr({OWNER_A}) AND EXISTS(n)", ["c", "d"]),
        (f"$creator = addr({OWNER_B})", ["c", "d"]),
        (f"$owner = addr({OWNER_C})", ["e"]),
        ("$expiresAt > u64(0)", ["a", "b", "c", "d", "e"]),
        ("$expiresAt < u64(1)", []),
        ("$expiresAt BETWEEN u64(100) AND u64(101)", ["a", "b", "c", "d", "e"]),
    ],
)
def test_query_language_matrix(qdata: MockArkiv, q: str, expected: list[str]) -> None:
    assert ids(qdata, q) == expected, q


def test_query_key_filters(qdata: MockArkiv) -> None:
    m = qdata
    allk = m.keys("$expiresAt > u64(0) ORDER BY $key ASC")
    assert len(allk) == 5
    assert m.keys(f"$key = key({allk[2]}) ORDER BY $key ASC") == [allk[2]]
    assert m.keys(f"$key > key({allk[2]}) ORDER BY $key ASC") == allk[3:]
    assert m.keys(f"$key <= key({allk[1]}) ORDER BY $key ASC") == allk[:2]
    assert m.keys(f"$key != key({allk[0]}) ORDER BY $key DESC") == list(reversed(allk[1:]))
    assert m.keys(f"$key BETWEEN key({allk[1]}) AND key({allk[3]}) ORDER BY $key ASC") == allk[1:4]


def test_owner_literal_checksum_enforced(qdata: MockArkiv) -> None:
    m = qdata
    good = "0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045"
    assert count(m, f"$owner = addr({good})") == 0  # valid checksum accepted
    assert count(m, f"$owner = addr({good.lower()})") == 0  # all-lowercase accepted
    bad = good[:-1] + ("6" if good[-1] != "6" else "7")
    bad = "0xd8Da6BF26964aF9D7eEd9e03E53415D37aA96045"  # flipped case -> checksum mismatch
    assert err_code(m, "arkiv_query", [f"$owner = addr({bad})"]) == -32003


def test_select_projection_and_count(qdata: MockArkiv) -> None:
    m = qdata
    # default: key only
    r = m.query("n = 5")
    assert r["data"] == [{"key": r["data"][0]["key"]}] and r["blockNumber"] == "0x1" and "count" not in r
    full = m.query(
        "n = 5",
        select={"owner": True, "creator": True, "createdAt": True, "updatedAt": True, "expiresAt": True, "creationFlags": True, "attributes": True},
    )["data"][0]
    assert set(full) == {"key", "owner", "creator", "createdAt", "updatedAt", "expiresAt", "creationFlags", "attributes"}
    assert full["owner"] == "0x" + "aa" * 20 or full["owner"].lower() == OWNER_A
    assert full["expiresAt"] == "0x65"
    names = {a["name"]: a for a in full["attributes"]}
    assert names["n"] == {"name": "n", "type": "i32", "value": 5}
    assert names["d"]["type"] == "dec" and names["d"]["value"] == "2.500000000000000000"
    assert names["b"]["value"] is False and names["blob"]["value"] == "0x02"
    # subset of attributes
    sub = m.query("n = 5", select={"attributes": {"n": True, "s": True, "d": False}})["data"][0]
    assert [a["name"] for a in sub["attributes"]] == ["n", "s"]
    # key can be dropped
    nokey = m.query("n = 5", select={"key": False, "owner": True})["data"][0]
    assert set(nokey) == {"owner"}
    # attributeSchema
    sch = m.query("n = 5", select={"attributeSchema": True})["data"][0]["attributeSchema"]
    assert {"name": "hidden", "type": "str", "indexed": False} in sch
    # count + data
    both = m.query("EXISTS(n)", select={"count": True, "owner": True}, limit="0x2")
    assert both["count"] == "0x4" and len(both["data"]) == 2
    # headless count
    cnt = m.query("EXISTS(n)", select={"count": True}, limit="0x1")
    assert cnt["data"] == [] and cnt["count"] == "0x4"
    assert m.query("n = 999", select={"count": True})["count"] == "0x0"
    assert m.query("n = 999")["data"] == []


def test_getentity_matches_query_projection(qdata: MockArkiv) -> None:
    m = qdata
    k = m.keys("n = 5")[0]
    e = m.rpc("arkiv_getEntity", [k])
    assert e["key"] == k and e["expiresAt"] == "0x65"
    assert m.rpc("arkiv_getEntity", ["0x" + "ab" * 32]) is None


@pytest.mark.parametrize(
    "q,code",
    [
        ("n = ", -32001),
        ("", -32001),
        ("n = 1 AND", -32001),
        ("(n = 1", -32001),
        ("n = 1)", -32001),
        ("n == 1", -32001),
        ("n = 'str'", -32001),  # untyped string literal
        ("n = 1.5", -32001),  # untyped decimal literal
        ("n = 1 ORDER n:i32", -32001),
        ("n = str('unterminated", -32001),
        ("n @ 1", -32001),
        ("$bogus = 1", -32001),
        ("n = i32(3000000000)", -32003),
        ("n = i32(-2147483649)", -32003),
        ("n = u64(18446744073709551616)", -32003),
        ("n = u64(-1)", -32003),
        ("k = key(0x1234)", -32003),
        ("a = addr(0x1234)", -32003),
        ("d = dec(abc)", -32001),
        ("n STARTSWITH i32(1)", -32002),
        ("$owner STARTSWITH str('x')", -32002),
        ("$key = addr(0x" + "11" * 20 + ")", -32002),
        ("$expiresAt = i32(1)", -32002),
        ("$createdAt = u64(1)", -32002),  # not filterable
        ("n BETWEEN i32(1) AND u64(5)", -32002),
        ("b = bytes(0x00)", -32002),  # bytes cannot be filtered
    ],
)
def test_query_error_codes(mock_arkiv: MockArkiv, q: str, code: int) -> None:
    status, out, _ = mock_arkiv.rpc_raw("arkiv_query", [q])
    assert status == 200 and out["error"]["code"] == code, (q, out)
    assert "data" in out["error"] or code == -32001


def test_query_parse_error_position(mock_arkiv: MockArkiv) -> None:
    with pytest.raises(JsonRpcError) as ei:
        mock_arkiv.rpc("arkiv_query", ["n = 1 AND"])
    assert ei.value.data["position"] == 9


def test_query_resource_limits(mock_arkiv_factory: Any) -> None:
    m: MockArkiv = mock_arkiv_factory("--max-query-bytes", "40", "--max-depth", "3", "--max-conditions", "4")
    assert m.query("n = 1")["data"] == []
    cases = (
        ("n = 1 AND " * 4 + "n = 1", "max_query_length"),  # 50 bytes > 40
        ("(((((n=1)))))", "max_nesting_depth"),
        ("NOT NOT NOT NOT n=1", "max_nesting_depth"),
        ("n=1 OR n=1 OR n=1 OR n=1", "max_nesting_depth"),  # 4-term chain = depth 4 > 3
    )
    for q, limit in cases:
        with pytest.raises(JsonRpcError) as ei:
            m.rpc("arkiv_query", [q])
        assert ei.value.code == -32004 and ei.value.data["limit"] == limit, (q, ei.value)
    assert m.query("n=1 OR n=2 OR n=3")["data"] == []  # depth exactly 3 is fine


def test_query_condition_limit(mock_arkiv_factory: Any) -> None:
    m: MockArkiv = mock_arkiv_factory("--max-conditions", "4")
    assert m.query("n=1 OR n=2 OR n=3 OR n=4")["data"] == []  # exactly at the limit
    with pytest.raises(JsonRpcError) as ei:
        m.rpc("arkiv_query", ["n=1 OR n=2 OR n=3 OR n=4 OR n=5"])
    assert ei.value.code == -32004 and ei.value.data == {"limit": "max_conditions", "maxAllowed": 4, "actual": 5}


def test_query_limits_exact_query_size_boundary(mock_arkiv_factory: Any) -> None:
    m: MockArkiv = mock_arkiv_factory("--max-query-bytes", "20")
    q20 = "n = 1 OR n = 2  --abc"  # 21 bytes
    q = "n = 1 OR n = 2 --abc"[:20]
    assert len(q.encode()) == 20
    m.query(q)
    with pytest.raises(JsonRpcError) as ei:
        m.rpc("arkiv_query", [q20])
    assert ei.value.code == -32004 and ei.value.data["actual"] == 21 and ei.value.data["maxAllowed"] == 20


def test_default_depth_limit_rejects_65_term_chain(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    ok = " AND ".join(["n = 1"] * 64)
    assert m.query(ok)["data"] == []
    with pytest.raises(JsonRpcError) as ei:
        m.rpc("arkiv_query", [" AND ".join(["n = 1"] * 66)])
    assert ei.value.code == -32004


def test_attribute_values_wire_encoding(mock_arkiv: MockArkiv) -> None:
    m = mock_arkiv
    k = m.create(
        min_lifetime=50,
        attributes=[
            attr("u", "u64", "0x10"),
            attr("big", "u256", 2**256 - 1),
            attr("neg", "i32", -7),
            attr("wallet", "addr", OWNER_A),
            attr("ref", "key", "0x" + "12" * 32),
        ],
    )
    m.mine()
    by = {a["name"]: a for a in m.rpc("arkiv_getEntity", [k])["attributes"]}
    assert by["u"]["value"] in (16, "0x10")
    assert by["neg"]["value"] == -7
    assert by["ref"]["value"] == "0x" + "12" * 32
    assert str(by["big"]["value"]) in (str(2**256 - 1), hex(2**256 - 1))
    # bad attributes rejected by control API (HTTP 400)
    for bad in (
        attr("n", "i32", 2**31),
        attr("n", "nosuchtype", 1),
    ):
        assert m.ctl_status("POST", "/entities", {"min_lifetime": 5, "attributes": [bad]})[0] == 400


_REVERT_NAME_BUG = (
    "MOCK BUG state.py:348 / state.py:334: Revert(..., name=name) collides with Revert.__init__(self, name, **args) "
    "-> TypeError -> HTTP 500 instead of a 409 revert (README: 'indexed true is rejected' for bytes)"
)


def test_indexed_bytes_attribute_is_rejected_cleanly(mock_arkiv: MockArkiv) -> None:
    status, body = mock_arkiv.ctl_status(
        "POST", "/entities", {"min_lifetime": 5, "attributes": [attr("blob", "bytes", "0x00", indexed=True)]}
    )
    assert status in (400, 409), (status, body)


def test_tombstone_in_create_is_rejected_cleanly(mock_arkiv: MockArkiv) -> None:
    status, body = mock_arkiv.ctl_status(
        "POST", "/entities", {"min_lifetime": 5, "attributes": [{"name": "x", "type": "tombstone"}]}
    )
    assert status in (400, 409), (status, body)


def test_dec_trim_flag(mock_arkiv_factory: Any) -> None:
    for flag, expect in (([], "4.500000000000000000"), (["--dec-trim"], "4.5")):
        m: MockArkiv = mock_arkiv_factory(*flag)
        k = m.create(min_lifetime=10, attributes=[attr("score", "dec", "4.5")])
        m.mine()
        assert m.rpc("arkiv_getEntity", [k])["attributes"][0]["value"] == expect


def test_auto_mine_produces_blocks(mock_arkiv: MockArkiv) -> None:
    import time

    m = mock_arkiv
    m.ctl("POST", "/auto_mine", {"interval_ms": 50})
    deadline = time.time() + 10
    while time.time() < deadline and m.head() < 3:
        time.sleep(0.05)
    m.ctl("POST", "/auto_mine", {"interval_ms": 0})
    assert m.head() >= 3
    h = m.head()
    time.sleep(0.3)
    assert m.head() == h  # stopped
