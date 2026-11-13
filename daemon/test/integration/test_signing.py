"""Signing with the user's own wallet, keystores and remote signers (GitHub issue #12, CCR-0001).

Client-side signing (no key on the daemon):
- entities:prepare returns complete EIP-1559 transactions (type, chain_id, from, gas, fee suggestion);
- entities:submit relays transactions the client signed (signed_transactions) and waits for receipts
  and indexing; created keys come from the receipt logs and match the predicted keys;
- entities:submit also waits for transactions a wallet broadcast itself (tx_hashes, MetaMask style);
- invalid, foreign-chain, non-Arkiv and non-writer transactions are rejected before broadcast.
Server-side signers:
- [write.signer] kind = "keystore" (passphrase file or systemd credential), wrong passphrase stops startup;
- kind = "remote" (Web3Signer eth_signTransaction and Clef account_signTransaction); a signer that
  changes the transaction or signs with another key is rejected;
- GET /v1/version reports writes_enabled and the signer kind and address, never key material.
"""

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Iterator

import pytest
from conftest import DEV_ADDR, DEV_KEY, DaemonClient, DaemonFactory, MockArkiv
from eth_sign import address_of, sign_tx, write_keystore

# Hardhat account #1: the end user's wallet (never holds real funds).
USER_KEY = bytes.fromhex("59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d")
USER_ADDR = address_of(USER_KEY)
OTHER_KEY = bytes.fromhex("5de4111afa1a4b94908f83103eb1f1706367c2e68ca870fc3fb9a804cdab365a")
DEV_PRIV = bytes.fromhex(DEV_KEY[2:])
CONTRACT = "0x4400000000000000000000000000000000000044"


def make_collection(d: DaemonClient, name: str, writers: list[str]) -> None:
    status, body = d.post(
        "/v1/collections",
        {
            "name": name,
            "column": {"type": "vector", "dimensions": 3},
            "index": {"method": "none", "opclass": "vector_l2_ops"},
            "writers": writers,
            "default_ttl": {"blocks": 100000},
        },
    )
    assert status == 201, body


def prepare(d: DaemonClient, col: str, sender: str, ids: list[str]) -> dict[str, Any]:
    ents = [{"id": i, "vector": [1, 2, 3], "document": f"doc {i}"} for i in ids]
    status, body = d.post(f"/v1/collections/{col}/entities:prepare", {"from": sender, "entities": ents})
    assert status == 200, body
    return body


@pytest.fixture
def readonly(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> DaemonClient:
    """A daemon with NO signing key, and a collection the user's wallet may write to."""
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    d = daemon_factory.start(signer=False)
    make_collection(d, "mine", [USER_ADDR])
    return d


# --------------------------------------------------------------------------- client-side signing


def test_version_without_signer(readonly: DaemonClient) -> None:
    status, v = readonly.get("/v1/version")
    assert status == 200
    assert v["writes_enabled"] is False
    assert v["signer"] is None
    assert "entities.prepare" in v["capabilities"] and "entities.submit" in v["capabilities"]
    status, body = readonly.post("/v1/collections/mine/entities", {"entities": [{"id": "x", "vector": [1, 2, 3]}]})
    assert status == 403 and body["code"] == "write_disabled"


def test_prepare_returns_signable_transactions(readonly: DaemonClient) -> None:
    body = prepare(readonly, "mine", USER_ADDR, ["a"])
    assert body["from"].lower() == USER_ADDR
    fees = body["fees"]
    assert fees["source"] in ("fee_history", "gas_price", "static")
    assert int(fees["max_fee_per_gas"], 16) >= int(fees["max_priority_fee_per_gas"], 16) > 0
    tx = body["transactions"][0]
    assert tx["type"] == 2 and tx["chain_id"] == 7794095 == body["chain_id"]
    assert tx["from"].lower() == USER_ADDR and tx["to"].lower() == CONTRACT
    assert tx["gas"] == tx["gas_estimate"] > 21000
    assert tx["max_fee_per_gas"] == fees["max_fee_per_gas"]
    assert tx["value"] == "0x0" and tx["data"].startswith("0x49650044")


def test_submit_client_signed_wait_indexed(readonly: DaemonClient) -> None:
    body = prepare(readonly, "mine", USER_ADDR, ["fish", "bird"])
    predicted = [r["key"] for r in body["results"]]
    raws = [sign_tx(USER_KEY, tx) for tx in body["transactions"]]
    status, res = readonly.post(
        "/v1/collections/mine/entities:submit", {"signed_transactions": raws, "wait": "indexed"}
    )
    assert status == 200, res
    assert len(res["tx_hashes"]) == len(raws)
    assert isinstance(res["block_number"], int) and res["block_number"] > 0
    assert [k.lower() for k in res["created_keys"]] == [k.lower() for k in predicted]
    # Indexed: visible without min_block, owned by the user's wallet.
    status, got = readonly.get("/v1/collections/mine/entities/fish?include=document")
    assert status == 200, got
    assert got["entity"]["owner"].lower() == USER_ADDR
    assert got["entity"]["document"] == "doc fish"
    status, s = readonly.post("/v1/collections/mine/search", {"vector": [1, 2, 3], "k": 5})
    assert status == 200 and {h["id"] for h in s["results"]} == {"fish", "bird"}


def test_submit_wait_none_then_hashes(readonly: DaemonClient, mock_arkiv: MockArkiv) -> None:
    body = prepare(readonly, "mine", USER_ADDR, ["cat"])
    raw = sign_tx(USER_KEY, body["transactions"][0])
    status, res = readonly.post("/v1/collections/mine/entities:submit", {"signed_transactions": [raw], "wait": "none"})
    assert status == 202, res
    assert res["block_number"] is None and res["created_keys"] == []
    # The same transaction, now waited for by hash.
    status, res2 = readonly.post(
        "/v1/collections/mine/entities:submit", {"tx_hashes": res["tx_hashes"], "wait": "indexed"}
    )
    assert status == 200, res2
    assert res2["created_keys"][0].lower() == body["results"][0]["key"].lower()
    assert readonly.get("/v1/collections/mine/entities/cat")[0] == 200


def test_submit_wallet_broadcast_tx_hashes(readonly: DaemonClient, mock_arkiv: MockArkiv) -> None:
    """MetaMask style: the wallet broadcasts (eth_sendTransaction) and the app only has the hash."""
    body = prepare(readonly, "mine", USER_ADDR, ["dog"])
    raw = sign_tx(USER_KEY, body["transactions"][0])
    tx_hash = mock_arkiv.rpc("eth_sendRawTransaction", [raw])
    status, res = readonly.post(
        "/v1/collections/mine/entities:submit", {"tx_hashes": [tx_hash.upper().replace("0X", "0x")], "wait": "indexed"}
    )
    assert status == 200, res
    assert res["tx_hashes"] == [tx_hash.lower()]
    assert res["created_keys"][0].lower() == body["results"][0]["key"].lower()
    status, got = readonly.get("/v1/collections/mine/entities/dog")
    assert status == 200 and got["entity"]["owner"].lower() == USER_ADDR


def test_submit_rejections(readonly: DaemonClient, mock_arkiv: MockArkiv) -> None:
    body = prepare(readonly, "mine", USER_ADDR, ["z"])
    tx = body["transactions"][0]
    head0 = mock_arkiv.head()

    def submit(payload: dict[str, Any]) -> tuple[int, Any]:
        return readonly.post("/v1/collections/mine/entities:submit", payload)

    status, res = submit({"signed_transactions": [sign_tx(USER_KEY, {**tx, "chain_id": 1})]})
    assert status == 422 and res["code"] == "invalid_argument" and "chain" in res["detail"], res
    status, res = submit({"signed_transactions": [sign_tx(USER_KEY, {**tx, "to": "0x" + "12" * 20})]})
    assert status == 422 and "Arkiv contract" in res["detail"], res
    status, res = submit({"signed_transactions": [sign_tx(USER_KEY, {**tx, "data": "0xdeadbeef"})]})
    assert status == 422 and "execute" in res["detail"], res
    status, res = submit({"signed_transactions": [sign_tx(OTHER_KEY, tx)]})
    assert status == 403 and res["code"] == "forbidden" and "not a writer" in res["detail"], res
    status, res = submit({"signed_transactions": ["0xf86c0985"]})
    assert status == 422 and "legacy" in res["detail"], res
    status, res = submit({"signed_transactions": ["0x02c0"]})
    assert status == 422, res
    good = sign_tx(USER_KEY, tx)
    status, res = submit({"signed_transactions": [good[:-2] + ("00" if good[-2:] != "00" else "01")]})
    assert status in (403, 422), res  # tampered signature: wrong sender or invalid
    status, res = submit({"signed_transactions": [good], "tx_hashes": ["0x" + "00" * 32]})
    assert status == 422, res
    status, res = submit({"tx_hashes": ["0x1234"]})
    assert status == 422, res
    status, res = submit({"signed_transactions": [good], "wait": "soon"})
    assert status == 422, res
    status, res = submit({"signed_transactions": [good], "extra": 1})
    assert status == 400, res
    status, res = readonly.post("/v1/collections/nope/entities:submit", {"signed_transactions": [good]})
    assert status == 404, res
    # Nothing above reached the chain.
    st = mock_arkiv.ctl("GET", "/state")
    assert not any(a["name"] == "av.id" and a["value"] == "z" for e in st["entities"] for a in e["attributes"])
    assert mock_arkiv.head() >= head0


def test_submit_hash_of_non_writer_is_forbidden(readonly: DaemonClient, mock_arkiv: MockArkiv) -> None:
    body = prepare(readonly, "mine", address_of(OTHER_KEY), ["q"])
    tx_hash = mock_arkiv.rpc("eth_sendRawTransaction", [sign_tx(OTHER_KEY, body["transactions"][0])])
    status, res = readonly.post("/v1/collections/mine/entities:submit", {"tx_hashes": [tx_hash], "wait": "receipt"})
    assert status == 403 and res["code"] == "forbidden", res


# --------------------------------------------------------------------------- keystore signer


def keystore_toml(path: Path, extra: str) -> str:
    return f'\n[write.signer]\nkind = "keystore"\nkeystore_path = "{path}"\n{extra}\n'


def test_keystore_signer_passphrase_file(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    wd = daemon_factory.new_dir()
    write_keystore(wd / "key.json", DEV_PRIV, "correct horse")
    (wd / "pass").write_text("correct horse\n")
    d = daemon_factory.start(
        workdir=wd, signer=False, extra_toml=keystore_toml(wd / "key.json", f'passphrase_file = "{wd / "pass"}"')
    )
    status, v = d.get("/v1/version")
    assert v["writes_enabled"] is True
    assert v["signer"] == {"kind": "keystore", "address": v["signer"]["address"]}
    assert v["signer"]["address"].lower() == DEV_ADDR
    assert "correct horse" not in json.dumps(v) and DEV_KEY[2:] not in daemon_factory.log(d)
    make_collection(d, "ks", [DEV_ADDR])
    status, res = d.post("/v1/collections/ks/entities", {"entities": [{"id": "k1", "vector": [1, 2, 3]}], "wait": "indexed"})
    assert status == 200, res
    assert d.get("/v1/collections/ks/entities/k1")[0] == 200


def test_keystore_signer_systemd_credential(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    wd = daemon_factory.new_dir()
    write_keystore(wd / "key.json", DEV_PRIV, "s3cret")
    creds = wd / "creds"
    creds.mkdir()
    (creds / "keystore-pass").write_text("s3cret")
    d = daemon_factory.start(
        workdir=wd,
        signer=False,
        env={"CREDENTIALS_DIRECTORY": str(creds)},
        extra_toml=keystore_toml(wd / "key.json", 'passphrase_credential = "keystore-pass"'),
    )
    assert d.get("/v1/version")[1]["signer"]["kind"] == "keystore"


def test_keystore_wrong_passphrase_stops_startup(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    wd = daemon_factory.new_dir()
    write_keystore(wd / "key.json", DEV_PRIV, "right")
    (wd / "pass").write_text("wrong")
    with pytest.raises(RuntimeError) as exc:
        daemon_factory.start(
            workdir=wd, signer=False, extra_toml=keystore_toml(wd / "key.json", f'passphrase_file = "{wd / "pass"}"')
        )
    assert "MAC mismatch" in str(exc.value)
    assert "wrong" not in str(exc.value).replace("passphrase incorrect", "")


def test_signer_config_errors(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    for toml, needle in [
        ('\n[write.signer]\nkind = "vault"\n', "kind"),
        ('\n[write.signer]\nkind = "keystore"\n', "keystore_path"),
        ('\n[write.signer]\nkind = "remote"\n', "remote_url"),
        ('\n[write.signer]\nkind = "env"\nunknown = 1\n', "unknown key"),
        ('\n[write.signer]\nkind = "none"\n[signer]\nkind = "none"\n', "not both"),
    ]:
        with pytest.raises(RuntimeError) as exc:
            daemon_factory.start(signer=False, extra_toml=toml)
        assert needle in str(exc.value), (toml, str(exc.value)[-800:])


def test_signer_kind_none_ignores_env_key(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    d = daemon_factory.start(signer=True, extra_toml='\n[signer]\nkind = "none"\n')
    v = d.get("/v1/version")[1]
    assert v["writes_enabled"] is False and v["signer"] is None


def test_env_signer_reports_kind(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    d = daemon_factory.start(signer=True)
    v = d.get("/v1/version")[1]
    assert v["writes_enabled"] is True
    assert v["signer"]["kind"] == "env" and v["signer"]["address"].lower() == DEV_ADDR


# --------------------------------------------------------------------------- remote signer


class RemoteSigner:
    """A Web3Signer / Clef stand-in that holds the key; modes simulate misbehaving signers."""

    def __init__(self, key: bytes, clef: bool = False) -> None:
        self.key = key
        self.clef = clef
        self.mode = "ok"  # ok | tamper | wrong_key | error
        self.calls: list[str] = []
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a: Any) -> None:
                pass

            def do_POST(self) -> None:
                req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                outer.calls.append(req["method"])
                out = {"jsonrpc": "2.0", "id": req["id"]}
                try:
                    out["result"] = outer.handle(req["method"], req.get("params", []))
                except Exception as exc:  # noqa: BLE001
                    out["error"] = {"code": -32000, "message": str(exc)}
                data = json.dumps(out).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def handle(self, method: str, params: list[Any]) -> Any:
        addr = address_of(self.key)
        if method in ("eth_accounts", "account_list"):
            return [addr]
        if method in ("eth_signTransaction", "account_signTransaction"):
            tx = dict(params[0])
            assert tx["from"].lower() == addr and tx["type"] == "0x2"
            if self.mode == "error":
                raise RuntimeError("signing refused by policy")
            if self.mode == "tamper":
                tx["gas"] = hex(int(tx["gas"], 16) + 1)
            raw = sign_tx(bytes.fromhex(OTHER_KEY.hex()) if self.mode == "wrong_key" else self.key, tx)
            return {"raw": raw, "tx": tx} if self.clef else raw
        raise RuntimeError(f"unsupported method {method}")

    def close(self) -> None:
        self.server.shutdown()


@pytest.fixture
def remote_signer() -> Iterator[Callable[..., RemoteSigner]]:
    made: list[RemoteSigner] = []

    def make(clef: bool = False) -> RemoteSigner:
        s = RemoteSigner(DEV_PRIV, clef)
        made.append(s)
        return s

    yield make
    for s in made:
        s.close()


@pytest.mark.parametrize("clef", [False, True])
def test_remote_signer(
    clef: bool, mock_arkiv: MockArkiv, daemon_factory: DaemonFactory, remote_signer: Callable[..., RemoteSigner]
) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    rs = remote_signer(clef)
    method = "account_signTransaction" if clef else "eth_signTransaction"
    d = daemon_factory.start(
        signer=False,
        extra_toml=f'\n[write.signer]\nkind = "remote"\nremote_url = "{rs.url}"\nremote_method = "{method}"\n',
    )
    v = d.get("/v1/version")[1]
    assert v["writes_enabled"] is True and v["signer"]["kind"] == "remote"
    assert v["signer"]["address"].lower() == DEV_ADDR
    assert ("account_list" if clef else "eth_accounts") in rs.calls
    make_collection(d, "rs", [DEV_ADDR])
    status, res = d.post(
        "/v1/collections/rs/entities", {"entities": [{"id": "r1", "vector": [1, 2, 3]}], "wait": "indexed"}
    )
    assert status == 200, res
    assert method in rs.calls
    status, got = d.get("/v1/collections/rs/entities/r1")
    assert status == 200 and got["entity"]["owner"].lower() == DEV_ADDR


def test_remote_signer_misbehaving(
    mock_arkiv: MockArkiv, daemon_factory: DaemonFactory, remote_signer: Callable[..., RemoteSigner]
) -> None:
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 100})
    rs = remote_signer()
    d = daemon_factory.start(
        signer=False,
        extra_toml=f'\n[write.signer]\nkind = "remote"\nremote_url = "{rs.url}"\nremote_address = "{DEV_ADDR}"\n',
    )
    assert "eth_accounts" not in rs.calls  # address configured: not asked
    make_collection(d, "rs", [DEV_ADDR])
    head_entities = len(mock_arkiv.ctl("GET", "/state")["entities"])
    for mode, needle in [
        ("tamper", "changed the transaction"),
        ("wrong_key", "different address"),
        ("error", "signing refused"),
    ]:
        rs.mode = mode
        status, res = d.post("/v1/collections/rs/entities", {"entities": [{"id": f"m-{mode}", "vector": [1, 2, 3]}]})
        assert status == 403 and res["code"] == "write_disabled" and needle in res["detail"], (mode, res)
    assert len(mock_arkiv.ctl("GET", "/state")["entities"]) == head_entities
    rs.mode = "ok"
    status, res = d.post("/v1/collections/rs/entities", {"entities": [{"id": "after", "vector": [1, 2, 3]}]})
    assert status == 200, res


def test_remote_signer_unreachable_stops_startup(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    with pytest.raises(RuntimeError) as exc:
        daemon_factory.start(
            signer=False, extra_toml='\n[write.signer]\nkind = "remote"\nremote_url = "http://127.0.0.1:9"\n'
        )
    assert "remote signer" in str(exc.value)
