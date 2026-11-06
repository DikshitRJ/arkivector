#!/usr/bin/env python3
"""mock-arkiv: deterministic simulator of an Arkiv node (JSON-RPC over HTTP/WS + /_mock control API).

Usage: python server.py --port P --seed N [--max-log-range 2000] [--auto-mine-ms 0] [--chain-id 0x76edaf]
Prints `READY <port>` on stdout once listening (`--port 0` picks a free port).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import sys
from typing import Any

from aiohttp import WSMsgType, web

import state as st
from faults import Faults
from logs import DEFAULT_CONTRACT, FilterError, Log, LogFilter, checksum_address, hx, qty
from query_lang import Limits, get_entity, run_query
from state import Chain, ControlError, Op, Revert, RpcError, parse_op

DEFAULT_OWNER_HEX = "0x" + "11" * 20


class Mock:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.chain = Chain(args.seed, args.chain_id, args.contract, args.max_history)
        self.faults = Faults(args.seed)
        self.limits = Limits(args.max_query_bytes, args.max_depth, args.max_conditions)
        self.max_log_range: int = args.max_log_range
        self.subs: dict[web.WebSocketResponse, dict[str, tuple[str, LogFilter | None]]] = {}
        self.sub_counter = 0
        self.auto_mine_ms = 0
        self.auto_task: asyncio.Task[None] | None = None

    # ------------------------------------------------------------------
    # Mining + pub/sub
    # ------------------------------------------------------------------

    async def mine(self, n: int) -> list[Any]:
        blocks = self.chain.mine(n)
        await self.publish([], blocks)
        return blocks

    async def publish(self, removed: list[Any], added: list[Any]) -> None:
        for ws in list(self.subs):
            for blk in removed:
                for tx in blk.txs:
                    for lg in tx.logs:
                        await self.push_log(ws, lg, removed=True)
            for blk in added:
                await self.push_block(ws, blk)
                for tx in blk.txs:
                    for lg in tx.logs:
                        await self.push_log(ws, lg, removed=False)

    async def _send(self, ws: web.WebSocketResponse, obj: Any) -> bool:
        if ws not in self.subs:
            return False
        if self.faults.should_drop_ws():
            await self.drop_ws(ws)
            return False
        try:
            await ws.send_str(json.dumps(obj))
        except (ConnectionResetError, RuntimeError) as exc:
            print(f"ws send failed, dropping subscriber: {exc}", file=sys.stderr, flush=True)
            await self.drop_ws(ws)
            return False
        return True

    async def drop_ws(self, ws: web.WebSocketResponse) -> None:
        self.subs.pop(ws, None)
        await ws.close()

    async def push_block(self, ws: web.WebSocketResponse, blk: Any) -> None:
        for sid, (kind, _) in list(self.subs.get(ws, {}).items()):
            if kind != "newHeads":
                continue
            hdr = blk.to_rpc(False, self.chain.chain_id)
            for k in ("transactions", "uncles", "size", "totalDifficulty"):
                hdr.pop(k, None)
            await self._send(ws, {"jsonrpc": "2.0", "method": "eth_subscription", "params": {"subscription": sid, "result": hdr}})

    async def push_log(self, ws: web.WebSocketResponse, lg: Log, removed: bool) -> None:
        for sid, (kind, flt) in list(self.subs.get(ws, {}).items()):
            if kind == "logs" and flt is not None and flt.matches(lg):
                await self._send(
                    ws, {"jsonrpc": "2.0", "method": "eth_subscription", "params": {"subscription": sid, "result": lg.to_rpc(removed)}}
                )

    async def auto_loop(self) -> None:
        while True:
            await asyncio.sleep(self.auto_mine_ms / 1000)
            await self.mine(1)

    def set_auto_mine(self, ms: int) -> None:
        if self.auto_task:
            self.auto_task.cancel()
            self.auto_task = None
        self.auto_mine_ms = ms
        if ms > 0:
            self.auto_task = asyncio.get_running_loop().create_task(self.auto_loop())

    # ------------------------------------------------------------------
    # JSON-RPC
    # ------------------------------------------------------------------

    def block_number(self, tag: Any, what: str = "block") -> int:
        if tag in ("latest", "pending", "safe", "finalized"):
            return self.chain.head
        if tag == "earliest":
            return 0
        if isinstance(tag, str) and re.fullmatch(r"0x[0-9a-fA-F]+", tag):
            return int(tag, 16)
        raise RpcError(-32602, "Invalid params", f"invalid {what} number or tag: {tag!r}")

    @staticmethod
    def hex_param(v: Any, nbytes: int | None, what: str) -> bytes:
        try:
            return st.parse_hex_bytes(v, nbytes, what)
        except ValueError as exc:
            raise RpcError(-32602, "Invalid params", str(exc)) from exc

    def call(self, method: str, params: Any) -> Any:
        c = self.chain
        if params is None:
            params = []
        if not isinstance(params, list):
            raise RpcError(-32602, "Invalid params", "params must be an array")
        if method == "arkiv_query":
            return run_query(c, params, self.limits)
        if method == "arkiv_getEntity":
            return get_entity(c, params)
        if method == "arkiv_getBlockTiming":
            if params:
                raise RpcError(-32602, "Invalid params", "expected no parameters")
            return {"current_block": c.head, "current_block_time": c.head_block.timestamp, "duration": st.BLOCK_DURATION}
        if method == "eth_chainId":
            return qty(c.chain_id)
        if method == "net_version":
            return str(c.chain_id)
        if method == "web3_clientVersion":
            return "mock-arkiv/0"
        if method == "eth_syncing":
            return False
        if method == "eth_gasPrice":
            return "0x3b9aca00"
        if method == "eth_blockNumber":
            return qty(c.head)
        if method in ("eth_getBlockByNumber", "eth_getBlockByHash"):
            if len(params) != 2 or not isinstance(params[1], bool):
                raise RpcError(-32602, "Invalid params", "expected [block, fullTransactions:boolean]")
            if method == "eth_getBlockByNumber":
                blk = c.block_by_number(self.block_number(params[0]))
            else:
                blk = c.block_by_hash(self.hex_param(params[0], 32, "block hash"))
            return blk.to_rpc(params[1], c.chain_id) if blk else None
        if method == "eth_getLogs":
            return self.get_logs(params)
        if method == "eth_getTransactionByHash":
            if len(params) != 1:
                raise RpcError(-32602, "Invalid params", "expected [hash]")
            tx = c.tx_index.get(self.hex_param(params[0], 32, "tx hash"))
            if tx is None:
                return None
            pending = tx.block_number == c.pending.number
            o = tx.to_rpc(pending)
            o["chainId"] = qty(c.chain_id)
            return o
        if method == "eth_getTransactionReceipt":
            if len(params) != 1:
                raise RpcError(-32602, "Invalid params", "expected [hash]")
            tx = c.tx_index.get(self.hex_param(params[0], 32, "tx hash"))
            if tx is None or tx.block_number > c.head:
                return None
            return tx.receipt()
        if method == "eth_getTransactionCount":
            if not 1 <= len(params) <= 2:
                raise RpcError(-32602, "Invalid params", "expected [address, block?]")
            a = self.hex_param(params[0], 20, "address")
            tag = params[1] if len(params) > 1 else "latest"
            table = c.acct_nonces if tag == "pending" else c.committed.acct_nonces
            return qty(table.get(a, 0))
        if method == "eth_call":
            if not params or not isinstance(params[0], dict):
                raise RpcError(-32602, "Invalid params", "expected [callObject, block?]")
            o = params[0]
            data = self.hex_param(o.get("data", o.get("input", "0x")), None, "data")
            return hx(c.eth_call(self.hex_param(o.get("to"), 20, "to"), data))
        if method == "eth_estimateGas":
            if not params or not isinstance(params[0], dict):
                raise RpcError(-32602, "Invalid params", "expected [txObject, block?]")
            data = self.hex_param(params[0].get("data", params[0].get("input", "0x")), None, "data")
            return qty(21000 + 16 * len(data) + 50000)
        if method == "eth_sendRawTransaction":
            raise RpcError(-32601, "not implemented in mock v0")
        if method in ("eth_subscribe", "eth_unsubscribe"):
            raise RpcError(-32601, f"{method} requires a WebSocket connection")
        raise RpcError(-32601, "Method not found", method)

    def get_logs(self, params: list[Any]) -> list[Any]:
        if len(params) != 1 or not isinstance(params[0], dict):
            raise RpcError(-32602, "Invalid params", "expected [filterObject]")
        f = params[0]
        try:
            flt = LogFilter.parse(f.get("address"), f.get("topics"))
        except FilterError as exc:
            raise RpcError(-32602, "Invalid params", str(exc)) from exc
        c = self.chain
        if f.get("blockHash") is not None:
            blk = c.block_by_hash(self.hex_param(f["blockHash"], 32, "blockHash"))
            lo = hi = blk.number if blk else -1
        else:
            lo = self.block_number(f.get("fromBlock", "latest"), "fromBlock")
            hi = self.block_number(f.get("toBlock", "latest"), "toBlock")
            if hi >= lo and hi - lo + 1 > self.max_log_range:
                raise RpcError(-32005, "log range exceeded", {"maxRange": self.max_log_range, "requested": hi - lo + 1})
        return [lg.to_rpc() for lg in c.logs_in_range(lo, min(hi, c.head)) if flt.matches(lg)]

    async def handle_one(self, req: Any) -> dict[str, Any] | None:
        if not isinstance(req, dict) or req.get("jsonrpc") != "2.0" or not isinstance(req.get("method"), str):
            return {"jsonrpc": "2.0", "id": req.get("id") if isinstance(req, dict) else None,
                    "error": RpcError(-32600, "Invalid Request").to_obj()}  # fmt: skip
        rid = req.get("id")
        method = req["method"]
        try:
            if self.faults.should_fail(method):
                raise RpcError(-32603, "Internal error", "injected fault (fail_methods)")
            result = self.call(method, req.get("params"))
        except RpcError as e:
            return {"jsonrpc": "2.0", "id": rid, "error": e.to_obj()}
        if "id" not in req:
            return None
        return {"jsonrpc": "2.0", "id": rid, "result": result}

    async def handle_payload(self, payload: Any) -> Any:
        if self.faults.latency_ms:
            await asyncio.sleep(self.faults.latency_ms / 1000)
        if isinstance(payload, list):
            if not payload:
                return {"jsonrpc": "2.0", "id": None, "error": RpcError(-32600, "Invalid Request").to_obj()}
            out = [r for r in [await self.handle_one(x) for x in payload] if r is not None]
            return out or None
        return await self.handle_one(payload)


# ----------------------------------------------------------------------
# HTTP / WS endpoints
# ----------------------------------------------------------------------


async def rpc_endpoint(request: web.Request) -> web.StreamResponse:
    m: Mock = request.app["mock"]
    if request.headers.get("Upgrade", "").lower() == "websocket":
        return await ws_endpoint(request)
    if request.method != "POST":
        return web.json_response({"error": "POST JSON-RPC to this endpoint"}, status=405)
    if m.faults.should_429():
        return web.json_response(
            {"error": "Too Many Requests: injected fault (http_429_ratio)"}, status=429, headers={"Retry-After": "1"}
        )
    try:
        payload = json.loads(await request.read())
    except (ValueError, UnicodeDecodeError):
        return web.json_response({"jsonrpc": "2.0", "id": None, "error": RpcError(-32700, "Parse error").to_obj()})
    out = await m.handle_payload(payload)
    if out is None:
        return web.Response(status=204)
    return web.json_response(out)


async def ws_endpoint(request: web.Request) -> web.StreamResponse:
    m: Mock = request.app["mock"]
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    m.subs[ws] = {}
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            if m.faults.should_drop_ws():
                await m.drop_ws(ws)
                break
            try:
                payload = json.loads(msg.data)
            except ValueError:
                await ws.send_str(json.dumps({"jsonrpc": "2.0", "id": None, "error": RpcError(-32700, "Parse error").to_obj()}))
                continue
            reqs = payload if isinstance(payload, list) else [payload]
            outs = []
            for r in reqs:
                if isinstance(r, dict) and r.get("method") in ("eth_subscribe", "eth_unsubscribe"):
                    if m.faults.latency_ms:
                        await asyncio.sleep(m.faults.latency_ms / 1000)
                    outs.append(ws_sub(m, ws, r))
                else:
                    outs.extend([x for x in [await m.handle_payload(r)] if x is not None])
            if outs:
                await ws.send_str(json.dumps(outs if isinstance(payload, list) else outs[0]))
    finally:
        m.subs.pop(ws, None)
    return ws


def ws_sub(m: Mock, ws: web.WebSocketResponse, req: dict[str, Any]) -> dict[str, Any]:
    rid = req.get("id")
    params = req.get("params")
    try:
        if not isinstance(params, list) or not params:
            raise RpcError(-32602, "Invalid params", "expected [kind, filter?] / [subscriptionId]")
        if req["method"] == "eth_unsubscribe":
            return {"jsonrpc": "2.0", "id": rid, "result": m.subs.get(ws, {}).pop(params[0], None) is not None}
        kind = params[0]
        if kind == "newHeads":
            entry: tuple[str, LogFilter | None] = ("newHeads", None)
        elif kind == "logs":
            f = params[1] if len(params) > 1 else {}
            if not isinstance(f, dict):
                raise RpcError(-32602, "Invalid params", "logs filter must be an object")
            try:
                entry = ("logs", LogFilter.parse(f.get("address"), f.get("topics")))
            except FilterError as exc:
                raise RpcError(-32602, "Invalid params", str(exc)) from exc
        else:
            raise RpcError(-32602, "Invalid params", "subscription kind must be newHeads or logs")
        m.sub_counter += 1
        sid = f"0x{m.sub_counter:032x}"
        m.subs[ws][sid] = entry
        return {"jsonrpc": "2.0", "id": rid, "result": sid}
    except RpcError as e:
        return {"jsonrpc": "2.0", "id": rid, "error": e.to_obj()}


# ----------------------------------------------------------------------
# Control API
# ----------------------------------------------------------------------


async def body_json(request: web.Request, allow_empty: bool = False) -> Any:
    raw = await request.read()
    if not raw.strip():
        if allow_empty:
            return {}
        raise ControlError("JSON body required")
    try:
        return json.loads(raw)
    except ValueError as exc:
        raise ControlError(f"invalid JSON: {exc}") from exc


def sender_of(raw: dict[str, Any]) -> bytes | None:
    s = raw.get("owner", raw.get("sender"))
    return None if s is None else st.parse_address(s, "owner")


def parse_tx(item: Any, kind: str | None = None) -> tuple[bytes | None, list[Op]]:
    """{owner?, ops:[...]} or a bare op object with optional owner."""
    if isinstance(item, dict) and "ops" in item:
        extra = set(item) - {"ops", "owner", "sender"}
        if extra:
            raise ControlError(f"unknown tx fields {sorted(extra)}")
        if not isinstance(item["ops"], list) or not item["ops"]:
            raise ControlError("ops must be a non-empty array")
        return sender_of(item), [parse_op(o) for o in item["ops"]]
    if not isinstance(item, dict):
        raise ControlError("expected an object")
    return sender_of(item), [parse_op(item, kind)]


def apply_summary(m: Mock, sender: bytes, ops: list[Op]) -> dict[str, Any]:
    logs = m.chain.apply_ops(sender, ops)
    return {
        "block": m.chain.pending.number,
        "txHash": logs[0].tx_hash,
        "sender": checksum_address(sender),
        "logs": [{"logIndex": lg.log_index, "topics": lg.topics, "data": lg.data} for lg in logs],
        "keys": [lg.topics[1] for lg in logs],
    }


async def c_create(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    body = await body_json(request)
    items = body if isinstance(body, list) else [body]
    parsed = []
    for it in items:
        s, ops = parse_tx(it, "create")
        parsed.append((s, ops))
    results = []
    for s, ops in parsed:
        try:
            results.append(apply_summary(m, m.chain.resolve_sender(s, ops), ops))
        except Revert as exc:
            return web.json_response({**exc.to_obj(), "applied": len(results), "created": results}, status=409)
    if isinstance(body, list):
        return web.json_response({"created": results})
    r = results[0]
    return web.json_response({"key": r["keys"][0], **r}, status=201)


def key_route(kind: str) -> Any:
    async def handler(request: web.Request) -> web.Response:
        m: Mock = request.app["mock"]
        body = await body_json(request, allow_empty=kind == "delete")
        if not isinstance(body, dict):
            raise ControlError("body must be an object")
        body = {**body, "key": request.match_info["key"]}
        s, ops = parse_tx(body, kind)
        return web.json_response(apply_summary(m, m.chain.resolve_sender(s, ops), ops))

    return handler


async def c_tx(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    s, ops = parse_tx(await body_json(request))
    return web.json_response(apply_summary(m, m.chain.resolve_sender(s, ops), ops))


def int_field(body: Any, name: str, default: int, lo: int, hi: int) -> int:
    if not isinstance(body, dict):
        raise ControlError("body must be an object")
    v = body.get(name, default)
    if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
        raise ControlError(f"{name} must be an integer in [{lo}, {hi}]")
    return v


async def c_mine(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    n = int_field(await body_json(request, True), "n", 1, 1, 100_000)
    blocks = await m.mine(n)
    return web.json_response({"mined": [b.number for b in blocks], "head": m.chain.head, "hash": hx(m.chain.head_block.hash)})


async def c_auto_mine(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    ms = int_field(await body_json(request), "interval_ms", 0, 0, 3_600_000)
    m.set_auto_mine(ms)
    return web.json_response({"auto_mine_ms": ms})


async def c_reorg(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    body = await body_json(request)
    depth = int_field(body, "depth", 1, 1, 100_000)
    extra = int_field(body, "extra_blocks", 0, 0, 100_000)
    repl_raw = body.get("replacement_ops", [])
    if not isinstance(repl_raw, list):
        raise ControlError("replacement_ops must be an array")
    repl = [parse_tx(x) for x in repl_raw]
    old_hashes = [hx(b.hash) for b in m.chain.blocks[-depth:]] if depth <= m.chain.head else []
    removed, added = m.chain.reorg(depth, repl, extra)  # raises ControlError/Revert, chain restored
    await m.publish(removed, added)
    return web.json_response(
        {
            "head": m.chain.head,
            "removed": [{"number": b.number, "hash": hx(b.hash)} for b in removed],
            "added": [{"number": b.number, "hash": hx(b.hash)} for b in added],
            "old_hashes": old_hashes,
            "epoch": m.chain.epoch,
        }
    )


async def c_faults(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    if request.method == "POST":
        m.faults.update(await body_json(request))
    return web.json_response(m.faults.to_obj())


async def c_state(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    c = m.chain
    q = request.query
    ents = list(c.committed.entities.values()) if q.get("all") else c.live_entities()
    ents.sort(key=lambda e: e.seq)
    out: dict[str, Any] = {
        "head": c.head,
        "hash": hx(c.head_block.hash),
        "timestamp": c.head_block.timestamp,
        "pending": {"block": c.pending.number, "txs": len(c.pending.txs)},
        "chainId": qty(c.chain_id),
        "contract": hx(c.contract),
        "seed": c.seed,
        "epoch": c.epoch,
        "auto_mine_ms": m.auto_mine_ms,
        "faults": m.faults.to_obj(),
        "live_count": len(c.live_entities()),
        "total_count": len(c.committed.entities),
    }
    out["entities"] = [hx(e.key) for e in ents] if q.get("keys") else [e.to_wire() for e in ents]
    return web.json_response(out)


async def c_reset(request: web.Request) -> web.Response:
    m: Mock = request.app["mock"]
    body = await body_json(request, True)
    seed = int_field(body, "seed", m.chain.seed, 0, 2**63)
    m.set_auto_mine(0)
    m.chain.reset(seed)
    m.faults.reseed(seed)
    if not body.get("keep_faults"):
        m.faults.clear()
    return web.json_response({"head": 0, "seed": seed})


@web.middleware
async def control_errors(request: web.Request, handler: Any) -> web.StreamResponse:
    try:
        return await handler(request)
    except ControlError as exc:
        return web.json_response({"error": "bad_request", "message": exc.message, "details": exc.details}, status=400)
    except Revert as exc:
        return web.json_response(exc.to_obj(), status=409)


def make_app(args: argparse.Namespace) -> web.Application:
    app = web.Application(middlewares=[control_errors], client_max_size=64 * 1024 * 1024)
    app["mock"] = Mock(args)
    r = app.router
    r.add_post("/_mock/entities", c_create)
    r.add_patch("/_mock/entities/{key}", key_route("patch"))
    r.add_delete("/_mock/entities/{key}", key_route("delete"))
    r.add_post("/_mock/entities/{key}/extend", key_route("extend"))
    r.add_post("/_mock/entities/{key}/transfer", key_route("transfer"))
    r.add_post("/_mock/tx", c_tx)
    r.add_post("/_mock/mine", c_mine)
    r.add_post("/_mock/auto_mine", c_auto_mine)
    r.add_post("/_mock/reorg", c_reorg)
    r.add_route("*", "/_mock/faults", c_faults)
    r.add_get("/_mock/state", c_state)
    r.add_post("/_mock/reset", c_reset)
    r.add_route("*", "/", rpc_endpoint)
    r.add_route("*", "/{access_key}", rpc_endpoint)
    return app


def int_arg(s: str) -> int:
    return int(s, 0)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--max-log-range", type=int, default=2000)
    p.add_argument("--auto-mine-ms", type=int, default=0)
    p.add_argument("--chain-id", type=int_arg, default=0x76EDAF)
    p.add_argument("--contract", default=DEFAULT_CONTRACT)
    p.add_argument("--max-query-bytes", type=int, default=16384)
    p.add_argument("--max-depth", type=int, default=64)
    p.add_argument("--max-conditions", type=int, default=1024)
    p.add_argument("--max-history", type=int, default=256, help="blocks of state history kept for reorgs")
    p.add_argument("--dec-trim", action="store_true", help='emit dec as "4.5" instead of "4.500000000000000000"')
    args = p.parse_args()
    st.DEC_TRIM = args.dec_trim

    async def run() -> None:
        app = make_app(args)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, args.host, args.port)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]  # type: ignore[union-attr]
        if args.auto_mine_ms > 0:
            app["mock"].set_auto_mine(args.auto_mine_ms)
        print(f"READY {port}", flush=True)
        await asyncio.Event().wait()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
