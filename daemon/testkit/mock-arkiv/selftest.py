#!/usr/bin/env python3
"""Quick self-check for mock-arkiv (stdlib only). Starts server.py as a subprocess.

Run: python selftest.py   (use the interpreter of a venv with requirements.txt installed)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
CONTRACT = "0x4400000000000000000000000000000000000044"
TOPICS = {
    "created": "0xb282d7c494b8899aa8015cd07be621530beb03409eb8c5e8fdc1411ba64356a5",
    "patched": "0xe986ee220b2d7947a009d9389d8400b2ac82d3b430a9f72700a0975d3d5a34fe",
    "extended": "0x10dc3526654c9ba4e2370b1bfcbc0de087d141a5694b3d3305d37df7e6705d4b",
    "transferred": "0x0b659dccc8eb950324170e8d9598af5ee04ee070883eb28651a96788721fbf83",
    "deleted": "0x4059b76c47e1ecc40ba88e649b654f716515eb358c3ce83c803820e3b3130cc3",
}
checks = 0


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        raise AssertionError(msg)
    print(f"ok   {msg}")


class Client:
    def __init__(self, base: str) -> None:
        self.base = base
        self.n = 0

    def http(self, method: str, path: str, body: Any = None) -> tuple[int, Any]:
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"null")

    def rpc(self, method: str, *params: Any) -> dict[str, Any]:
        self.n += 1
        _, out = self.http("POST", "/", {"jsonrpc": "2.0", "id": self.n, "method": method, "params": list(params)})
        return out

    def ok(self, method: str, *params: Any) -> Any:
        out = self.rpc(method, *params)
        assert "result" in out, out
        return out["result"]

    def mock(self, method: str, path: str, body: Any = None) -> Any:
        code, out = self.http(method, "/_mock" + path, body)
        assert code < 300, (code, out)
        return out


def main() -> int:
    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "server.py"), "--port", "0", "--seed", "7"],
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        assert proc.stdout is not None
        line = proc.stdout.readline().strip()
        assert line.startswith("READY "), line
        c = Client(f"http://127.0.0.1:{int(line.split()[1])}")
        check(c.ok("eth_chainId") == "0x76edaf", "eth_chainId = 0x76edaf")

        # Key derivation against the worked example in entity-key-derivation.md 2.2.
        r = c.mock("POST", "/entities", {"owner": "0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045", "min_lifetime": 50})
        check(r["key"] == "0x4b18bfc16927943dd824368476f0acf1f2d1f60cec6534f41a0e98a9d8f754b3", "doc key-derivation example reproduced")

        # 250 entities, bulk create
        items = [
            {"attributes": [{"name": "arkiv.schema", "type": "str", "value": "t.docs"},
                            {"name": "n", "type": "i32", "value": i},
                            {"name": "score", "type": "dec", "value": f"{i % 7}.5"}]}
            for i in range(250)
        ]  # fmt: skip
        created = {x["keys"][0] for x in c.mock("POST", "/entities", items)["created"]}
        check(len(created) == 250, "250 distinct keys created")
        check(c.ok("eth_blockNumber") == "0x0", "pending txs are invisible before mining")
        c.mock("POST", "/mine", {"n": 1})

        # Key-cursor pagination returns each entity exactly once.
        seen: list[str] = []
        cursor = None
        while True:
            q = "arkiv.schema = str('t.docs')" + (f" AND $key > key({cursor})" if cursor else "") + " ORDER BY $key ASC"
            page = c.ok("arkiv_query", q, {"select": {"key": True}, "limit": "0x64"})["data"]
            seen += [e["key"] for e in page]
            if len(page) < 100:
                break
            cursor = page[-1]["key"]
        check(len(seen) == 250 and set(seen) == created and seen == sorted(seen), "cursor pagination: each of 250 exactly once, sorted")

        # limit bounds
        for bad in ("0x65", "0x0", 100):
            out = c.rpc("arkiv_query", "arkiv.schema = str('t.docs')", {"limit": bad})
            check(out.get("error", {}).get("code") == -32602, f"limit {bad!r} -> -32602")

        # Count, projection, ordering with attribute types
        res = c.ok("arkiv_query", "n >= 10 AND n < i32(20)", {"select": {"count": True}})
        check(res["data"] == [] and res["count"] == hex(10), "headless count")
        res = c.ok("arkiv_query", "arkiv.schema = str('t.docs') ORDER BY n:i32 DESC", {"select": {"attributes": {"n": True}}, "limit": "0x3"})
        check([e["attributes"][0]["value"] for e in res["data"]] == [249, 248, 247], "ORDER BY n:i32 DESC")
        res = c.ok("arkiv_query", "n = 5 OR n BETWEEN 7 AND 8 OR NOT (EXISTS(n) OR score STARTSWITH str('x'))", {"select": {"count": True}})
        check(res["count"] == "0x4", "OR / BETWEEN / NOT / EXISTS")
        check(c.rpc("arkiv_query", "n = i32(3000000000)")["error"]["code"] == -32003, "literal overflow -> -32003")
        check(c.rpc("arkiv_query", "n = ")["error"]["code"] == -32001, "syntax error -> -32001")
        check(c.rpc("arkiv_query", "n STARTSWITH i32(1)")["error"]["code"] == -32002, "STARTSWITH non-str -> -32002")
        check(c.rpc("arkiv_query", "n = 1 AND " * 2000 + "n = 1")["error"]["code"] == -32004, "query too long -> -32004")

        # TTL: entity created in block B expires (hidden) at block B + ttl
        head = int(c.ok("eth_blockNumber"), 16)
        k = c.mock("POST", "/entities", {"min_lifetime": 3, "attributes": [{"name": "t", "type": "str", "value": "ttl"}]})["key"]
        c.mock("POST", "/mine", {"n": 1})  # creation block = head+1, expiry = head+4
        exp = int(c.ok("arkiv_getEntity", k)["expiresAt"], 16)
        check(exp == head + 4, f"expiresAt = creation block + minLifetime ({exp})")
        c.mock("POST", "/mine", {"n": 2})  # head+3 = expiresAt-1
        live = c.ok("arkiv_query", "t = str('ttl')", {"select": {"count": True}})["count"]
        check(live == "0x1" and c.ok("arkiv_getEntity", k) is not None, "visible at block expiresAt-1")
        c.mock("POST", "/mine", {"n": 1})
        live = c.ok("arkiv_query", "t = str('ttl')", {"select": {"count": True}})["count"]
        check(live == "0x0" and c.ok("arkiv_getEntity", k) is None, "hidden at block expiresAt")

        # Logs for each of the 5 ops
        k2 = c.mock("POST", "/entities", {"owner": "0x" + "22" * 20, "min_lifetime": 100})["key"]
        c.mock("PATCH", f"/entities/{k2}", {"attributes": [{"name": "a", "type": "bool", "value": True}]})
        c.mock("POST", f"/entities/{k2}/extend", {"min_lifetime": 200})
        c.mock("POST", f"/entities/{k2}/transfer", {"new_owner": "0x" + "33" * 20})
        c.mock("DELETE", f"/entities/{k2}")
        c.mock("POST", "/mine", {"n": 1})
        h = c.ok("eth_blockNumber")
        logs = c.ok("eth_getLogs", {"fromBlock": h, "toBlock": h, "address": CONTRACT})
        check([lg["topics"][0] for lg in logs] == [TOPICS[x] for x in ("created", "patched", "extended", "transferred", "deleted")], "5 events in op order")
        check([int(lg["logIndex"], 16) for lg in logs] == [0, 1, 2, 3, 4], "logIndex sequential in block")
        check(logs[3]["topics"][3].endswith("33" * 20) and logs[3]["topics"][2].endswith("22" * 20), "OwnershipTransferred topics")
        check(c.ok("eth_getLogs", {"fromBlock": h, "toBlock": h, "topics": [TOPICS["deleted"]]})[0]["topics"][1] == k2, "topic0 filter")
        check(c.ok("eth_getLogs", {"fromBlock": h, "toBlock": h, "address": "0x" + "55" * 20}) == [], "address filter excludes")
        out = c.rpc("eth_getLogs", {"fromBlock": "0x0", "toBlock": "0x7d1"})
        check(out["error"]["code"] == -32005 if int(h, 16) >= 2000 else True, "log range cap")
        tx = c.ok("eth_getTransactionByHash", logs[0]["transactionHash"])
        check(tx["input"].startswith("0x49650044"), "tx calldata uses execute selector")

        # Reorg changes hashes
        before = c.ok("eth_getBlockByNumber", h, False)["hash"]
        c.mock("POST", "/reorg", {"depth": 2})
        after = c.ok("eth_getBlockByNumber", h, False)["hash"]
        check(before != after and c.ok("eth_blockNumber") == h, "reorg keeps height, changes hash")

        # Faults
        c.mock("POST", "/faults", {"http_429_ratio": 1.0})
        code, _ = c.http("POST", "/", {"jsonrpc": "2.0", "id": 1, "method": "eth_blockNumber", "params": []})
        check(code == 429, "http_429_ratio=1 -> 429")
        check(c.http("GET", "/_mock/state")[0] == 200, "control API unaffected by faults")
        c.mock("POST", "/faults", {"http_429_ratio": 0})
        check(c.rpc("eth_sendRawTransaction", "0x00")["error"]["code"] == -32601, "eth_sendRawTransaction -> -32601 (v0)")
        print(f"\nselftest PASSED ({checks} checks)")
        return 0
    finally:
        proc.terminate()
        proc.wait(timeout=5)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print(f"\nselftest FAILED: {e}")
        sys.exit(1)
