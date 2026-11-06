"""Arkiv query language: lexer, parser, semantic checks, evaluator and arkiv_query execution.

Grammar and error codes follow RESEARCH/arkiv-docs/04-query-engine-and-ast and
02-json-rpc-protocol/error-handling.md: -32001 parse, -32002 type, -32003 literal,
-32004 limits, -32602 invalid params.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

from logs import checksum_address, hx, is_valid_checksum, qty
from state import (
    I32_MAX,
    I32_MIN,
    MAX_STR_BYTES,
    RESERVED_NAMES,
    U64_MAX,
    U256_MAX,
    Chain,
    Entity,
    RpcError,
    parse_dec,
    sort_value,
    wire_value,
)

SYSTEM_FIELDS = {"$key": "key", "$owner": "addr", "$creator": "addr", "$expiresAt": "u64"}
UNFILTERABLE_SYSTEM = {"$createdAt", "$updatedAt", "$creationFlags"}
TYPE_NAMES = {"bool", "i32", "u64", "u256", "dec", "str", "addr", "key", "bytes"}
SORTABLE = TYPE_NAMES - {"bytes"}


@dataclass
class Limits:
    max_query_bytes: int = 16384
    max_depth: int = 64
    max_conditions: int = 1024


def parse_error(pos: int, expected: Any, found: str) -> RpcError:
    return RpcError(-32001, "Query parse error", {"position": pos, "expected": expected, "found": found})


def type_error(**data: Any) -> RpcError:
    return RpcError(-32002, "Type error in predicate", data)


def literal_error(typ: str, value: str, reason: str) -> RpcError:
    return RpcError(-32003, "Invalid literal value", {"type": typ, "value": value, "reason": reason})


def limit_error(name: str, max_allowed: int, actual: int) -> RpcError:
    return RpcError(-32004, "Node resource limits exceeded", {"limit": name, "maxAllowed": max_allowed, "actual": actual})


# --------------------------------------------------------------------------
# Lexer
# --------------------------------------------------------------------------

_IDENT = re.compile(r"[A-Za-z][A-Za-z0-9._-]*")
_NUM = re.compile(r"-?\d+(?:\.\d+)?")


@dataclass
class Tok:
    kind: str  # ident sys num str hex op punct eof
    text: str
    pos: int


def lex(q: str) -> list[Tok]:
    toks: list[Tok] = []
    i, n = 0, len(q)
    while i < n:
        c = q[i]
        if c in " \t\r\n":
            i += 1
        elif q.startswith("--", i):
            j = q.find("\n", i)
            i = n if j < 0 else j + 1
        elif c == "'":
            j, buf = i + 1, []
            while True:
                if j >= n:
                    raise parse_error(i, "closing quote", "<EOF>")
                if q[j] == "'":
                    if j + 1 < n and q[j + 1] == "'":
                        buf.append("'")
                        j += 2
                        continue
                    break
                buf.append(q[j])
                j += 1
            toks.append(Tok("str", "".join(buf), i))
            i = j + 1
        elif c == "$":
            m = re.compile(r"\$[A-Za-z]+").match(q, i)
            if not m:
                raise parse_error(i, "system field", "$")
            toks.append(Tok("sys", m.group(), i))
            i = m.end()
        elif c == "0" and q[i : i + 2].lower() == "0x":
            m = re.compile(r"0[xX][0-9a-fA-F]*").match(q, i)
            assert m
            toks.append(Tok("hex", m.group(), i))
            i = m.end()
        elif c.isdigit() or (c == "-" and i + 1 < n and q[i + 1].isdigit()):
            m = _NUM.match(q, i)
            assert m
            toks.append(Tok("num", m.group(), i))
            i = m.end()
        elif c.isalpha() and c.isascii():
            m = _IDENT.match(q, i)
            assert m
            text = m.group()
            k = text.find("--")  # a comment may start inside what looks like an identifier
            if k > 0:
                text = text[:k]
            toks.append(Tok("ident", text, i))
            i += len(text)
        elif q.startswith(("!=", "<=", ">="), i):
            toks.append(Tok("op", q[i : i + 2], i))
            i += 2
        elif c in "=<>":
            toks.append(Tok("op", c, i))
            i += 1
        elif c in "():,":
            toks.append(Tok("punct", c, i))
            i += 1
        else:
            raise parse_error(i, "token", c)
    toks.append(Tok("eof", "<EOF>", n))
    return toks


# --------------------------------------------------------------------------
# AST
# --------------------------------------------------------------------------
# ("cmp", ref, op, lit) ("between", ref, lo, hi) ("starts", ref, lit) ("exists", ref)
# ("typeof", ref, type) ("not", node) ("and", [nodes]) ("or", [nodes])
# ref = (name, is_system); lit = (type, internal_value)


@dataclass
class Order:
    ref: tuple[str, bool]
    type: str
    desc: bool


@dataclass
class Query:
    cond: Any
    order: Order | None


class Parser:
    def __init__(self, q: str, limits: Limits) -> None:
        self.q = q
        self.limits = limits
        self.toks = lex(q)
        self.i = 0
        self.conds = 0

    @property
    def t(self) -> Tok:
        return self.toks[self.i]

    def kw(self, word: str) -> bool:
        return self.t.kind == "ident" and self.t.text.lower() == word

    def eat_kw(self, word: str) -> bool:
        if self.kw(word):
            self.i += 1
            return True
        return False

    def punct(self, ch: str) -> bool:
        if self.t.kind == "punct" and self.t.text == ch:
            self.i += 1
            return True
        return False

    def need(self, ch: str) -> None:
        if not self.punct(ch):
            raise parse_error(self.t.pos, ch, self.t.text)

    def parse(self) -> Query:
        node, _ = self.or_expr(0)
        order = None
        if self.kw("order"):
            self.i += 1
            if not self.eat_kw("by"):
                raise parse_error(self.t.pos, "BY", self.t.text)
            order = self.order_clause()
        if self.t.kind != "eof":
            raise parse_error(self.t.pos, ["AND", "OR", "ORDER BY", "<EOF>"], self.t.text)
        return Query(node, order)

    # Each *_expr returns (node, depth) where depth models a left-associative binary AST so that
    # long AND/OR chains count against the nesting limit (error-handling.md 2.4).
    def or_expr(self, level: int) -> tuple[Any, int]:
        return self.chain("or", self.and_expr, level)

    def and_expr(self, level: int) -> tuple[Any, int]:
        return self.chain("and", self.unary, level)

    def chain(self, op: str, sub: Any, level: int) -> tuple[Any, int]:
        node, depth = sub(level)
        items = [node]
        while self.kw(op):
            self.i += 1
            nxt, d = sub(level)
            items.append(nxt)
            depth = 1 + max(depth, d)
            if depth > self.limits.max_depth:
                raise limit_error("max_nesting_depth", self.limits.max_depth, depth)
        self.last_depth = depth
        return (node if len(items) == 1 else (op, items)), depth

    def unary(self, level: int) -> tuple[Any, int]:
        if level + 1 > self.limits.max_depth:
            raise limit_error("max_nesting_depth", self.limits.max_depth, level + 1)
        if self.eat_kw("not"):
            node, d = self.unary(level + 1)
            return ("not", node), d + 1
        if self.punct("("):
            node, d = self.or_expr(level + 1)
            self.need(")")
            return node, d + 1
        return self.predicate(), 1

    def ref(self) -> tuple[str, bool]:
        t = self.t
        if t.kind == "sys":
            if t.text in SYSTEM_FIELDS:
                self.i += 1
                return (t.text, True)
            if t.text in UNFILTERABLE_SYSTEM:
                raise type_error(attribute=t.text, reason="system field is not filterable")
            raise parse_error(t.pos, "system field ($key|$owner|$creator|$expiresAt)", t.text)
        if t.kind == "ident":
            if t.text.lower() in RESERVED_NAMES:
                raise parse_error(t.pos, "attribute name", t.text)
            if len(t.text.encode()) > 32:
                raise parse_error(t.pos, "attribute name <= 32 bytes", t.text)
            self.i += 1
            return (t.text, False)
        raise parse_error(t.pos, "attribute name", t.text)

    def predicate(self) -> Any:
        self.conds += 1
        if self.conds > self.limits.max_conditions:
            raise limit_error("max_conditions", self.limits.max_conditions, self.conds)
        if self.kw("exists"):
            self.i += 1
            self.need("(")
            r = self.ref()
            self.need(")")
            return ("exists", r)
        if self.kw("typeof"):
            self.i += 1
            self.need("(")
            r = self.ref()
            self.need(")")
            if not (self.t.kind == "op" and self.t.text == "="):
                raise parse_error(self.t.pos, "=", self.t.text)
            self.i += 1
            if self.t.kind != "ident" or self.t.text not in TYPE_NAMES:
                raise parse_error(self.t.pos, sorted(TYPE_NAMES), self.t.text)
            typ = self.t.text
            self.i += 1
            return ("typeof", r, typ)
        r = self.ref()
        if self.eat_kw("between"):
            lo = self.literal()
            if not self.eat_kw("and"):
                raise parse_error(self.t.pos, "AND", self.t.text)
            hi = self.literal()
            self.check_ref_type(r, lo)
            if lo[0] != hi[0]:
                raise type_error(operator="BETWEEN", expectedType=lo[0], actualType=hi[0], attribute=r[0])
            self.no_bytes("BETWEEN", r, lo)
            return ("between", r, lo, hi)
        if self.eat_kw("startswith"):
            lit = self.literal()
            if lit[0] != "str" or r[1]:
                raise type_error(operator="STARTSWITH", expectedType="str", actualType=SYSTEM_FIELDS.get(r[0]) or lit[0], attribute=r[0])
            return ("starts", r, lit)
        if self.t.kind == "op":
            op = self.t.text
            self.i += 1
            lit = self.literal()
            self.check_ref_type(r, lit)
            self.no_bytes(op, r, lit)
            return ("cmp", r, op, lit)
        raise parse_error(self.t.pos, ["=", "!=", "<", "<=", ">", ">=", "BETWEEN", "STARTSWITH"], self.t.text)

    @staticmethod
    def no_bytes(op: str, r: tuple[str, bool], lit: tuple[str, Any]) -> None:
        if lit[0] == "bytes":
            raise type_error(operator=op, attribute=r[0], actualType="bytes", reason="bytes cannot be filtered")

    @staticmethod
    def check_ref_type(r: tuple[str, bool], lit: tuple[str, Any]) -> None:
        if r[1] and SYSTEM_FIELDS[r[0]] != lit[0]:
            raise type_error(attribute=r[0], expectedType=SYSTEM_FIELDS[r[0]], actualType=lit[0])

    def literal(self) -> tuple[str, Any]:
        t = self.t
        if t.kind == "num":
            self.i += 1
            if "." in t.text:
                raise parse_error(t.pos, "typed literal (e.g. dec(1.5))", t.text)
            return ("i32", self.int_lit("i32", t.text, I32_MIN, I32_MAX))
        if t.kind == "ident":
            low = t.text.lower()
            if low in ("true", "false"):
                self.i += 1
                return ("bool", low == "true")
            if t.text in TYPE_NAMES:
                self.i += 1
                self.need("(")
                lit = self.constructor(t.text)
                self.need(")")
                return lit
        raise parse_error(t.pos, "typed literal", t.text)

    def int_lit(self, typ: str, text: str, lo: int, hi: int) -> int:
        n = int(text)
        if not lo <= n <= hi:
            raise literal_error(typ, text, f"value outside [{lo}, {hi}]")
        return n

    def constructor(self, typ: str) -> tuple[str, Any]:
        t = self.t
        if typ == "bool":
            if t.kind == "ident" and t.text.lower() in ("true", "false"):
                self.i += 1
                return ("bool", t.text.lower() == "true")
            raise parse_error(t.pos, "true|false", t.text)
        if typ in ("i32", "u64", "u256"):
            if t.kind != "num" or "." in t.text:
                raise parse_error(t.pos, "integer", t.text)
            self.i += 1
            lo, hi = {"i32": (I32_MIN, I32_MAX), "u64": (0, U64_MAX), "u256": (0, U256_MAX)}[typ]
            return (typ, self.int_lit(typ, t.text, lo, hi))
        if typ == "dec":
            if t.kind not in ("num", "str"):
                raise parse_error(t.pos, "decimal number", t.text)
            self.i += 1
            try:
                return ("dec", parse_dec(t.text))
            except ValueError as exc:
                raise literal_error("dec", t.text, str(exc)) from exc
        if typ == "str":
            if t.kind != "str":
                raise parse_error(t.pos, "single-quoted string", t.text)
            self.i += 1
            if len(t.text.encode("utf-8")) > MAX_STR_BYTES:
                raise literal_error("str", t.text[:32] + "...", f"string exceeds {MAX_STR_BYTES} bytes")
            return ("str", t.text)
        if t.kind != "hex":
            raise parse_error(t.pos, "0x-prefixed hex", t.text)
        self.i += 1
        body = t.text[2:]
        if typ == "addr":
            if len(body) != 40:
                raise literal_error("addr", t.text, "address must be 20 bytes")
            if not is_valid_checksum(t.text):
                raise literal_error("addr", t.text, "EIP-55 checksum mismatch")
            return ("addr", bytes.fromhex(body))
        if typ == "key":
            if len(body) != 64:
                raise literal_error("key", t.text, "key must be 32 bytes")
            return ("key", bytes.fromhex(body))
        if len(body) % 2:
            raise literal_error("bytes", t.text, "odd hex length")
        return ("bytes", bytes.fromhex(body))

    def order_clause(self) -> Order:
        t = self.t
        if t.kind == "sys":
            if t.text not in SYSTEM_FIELDS:
                raise type_error(attribute=t.text, reason="system field is not sortable")
            ref, typ = (t.text, True), SYSTEM_FIELDS[t.text]
            self.i += 1
            if self.punct(":"):
                if self.t.kind != "ident" or self.t.text != typ:
                    raise type_error(attribute=t.text, expectedType=typ, actualType=self.t.text)
                self.i += 1
        else:
            ref = self.ref()
            typ = ""
            if not self.punct(":"):
                raise parse_error(self.t.pos, "':<type>'", self.t.text)
            if self.t.kind != "ident" or self.t.text not in TYPE_NAMES:
                raise parse_error(self.t.pos, sorted(TYPE_NAMES), self.t.text)
            typ = self.t.text
            if typ not in SORTABLE:
                raise type_error(attribute=ref[0], actualType=typ, reason="type is not sortable")
            self.i += 1
        desc = False
        if self.eat_kw("desc"):
            desc = True
        else:
            self.eat_kw("asc")
        return Order(ref, typ, desc)


def parse_query(q: str, limits: Limits) -> Query:
    n = len(q.encode("utf-8"))
    if n > limits.max_query_bytes:
        raise limit_error("max_query_length", limits.max_query_bytes, n)
    return Parser(q, limits).parse()


# --------------------------------------------------------------------------
# Evaluation
# --------------------------------------------------------------------------


def _get(e: Entity, ref: tuple[str, bool]) -> tuple[str, Any] | None:
    """(type, value) for matching purposes; unindexed user attributes are invisible to predicates."""
    name, system = ref
    if system:
        if name == "$key":
            return ("key", e.key)
        if name == "$owner":
            return ("addr", e.owner)
        if name == "$creator":
            return ("addr", e.creator)
        return ("u64", e.expires_at)
    a = e.attr(name)
    if a is None or not a.indexed:
        return None
    return (a.type, a.value)


def _cmp(op: str, a: Any, b: Any) -> bool:
    return {"=": a == b, "!=": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]


def evaluate(node: Any, e: Entity) -> bool:
    k = node[0]
    if k == "and":
        return all(evaluate(c, e) for c in node[1])
    if k == "or":
        return any(evaluate(c, e) for c in node[1])
    if k == "not":
        return not evaluate(node[1], e)
    got = _get(e, node[1])
    if k == "exists":
        return got is not None
    if k == "typeof":
        return got is not None and got[0] == node[2]
    if got is None:
        return False
    if k == "cmp":
        lt, lv = node[3]
        return got[0] == lt and _cmp(node[2], sort_value(lt, got[1]), sort_value(lt, lv))
    if k == "between":
        lt = node[2][0]
        if got[0] != lt:
            return False
        v = sort_value(lt, got[1])
        return sort_value(lt, node[2][1]) <= v <= sort_value(lt, node[3][1])
    if k == "starts":
        return got[0] == "str" and got[1].encode("utf-8").startswith(node[2][1].encode("utf-8"))
    raise AssertionError(k)


# --------------------------------------------------------------------------
# arkiv_query execution
# --------------------------------------------------------------------------

_QTY = re.compile(r"^0x[0-9a-fA-F]+$")
ENTITY_FIELDS = {"key", "owner", "creator", "createdAt", "updatedAt", "expiresAt", "creationFlags", "attributes", "attributeSchema"}


def _qty_param(v: Any, name: str, lo: int, hi: int | None) -> int:
    if not isinstance(v, str) or not _QTY.match(v):
        raise RpcError(-32602, "Invalid params", f"{name} must be a hex quantity string like '0x1'")
    n = int(v, 16)
    if n < lo or (hi is not None and n > hi):
        rng = f"between '0x{lo:x}' and '0x{hi:x}'" if hi is not None else f">= '0x{lo:x}'"
        raise RpcError(-32602, "Invalid params", f"{name} must be a hex string {rng}")
    return n


def parse_options(opts: Any) -> tuple[dict[str, Any], int, int]:
    if opts is None:
        opts = {}
    if not isinstance(opts, dict):
        raise RpcError(-32602, "Invalid params", "options must be an object")
    extra = set(opts) - {"select", "limit", "offset"}
    if extra:
        raise RpcError(-32602, "Invalid params", f"unknown option(s) {sorted(extra)}")
    limit = _qty_param(opts["limit"], "limit", 1, 100) if "limit" in opts else 100
    offset = _qty_param(opts["offset"], "offset", 0, None) if "offset" in opts else 0
    sel = opts.get("select") or {}
    if not isinstance(sel, dict):
        raise RpcError(-32602, "Invalid params", "select must be an object")
    for k, v in sel.items():
        if k == "count":
            if not isinstance(v, bool):
                raise RpcError(-32602, "Invalid params", "select.count must be boolean")
        elif k == "attributes":
            ok = isinstance(v, bool) or (isinstance(v, dict) and all(isinstance(x, bool) for x in v.values()))
            if not ok:
                raise RpcError(-32602, "Invalid params", "select.attributes must be boolean or {name: boolean}")
        elif k in ENTITY_FIELDS:
            if not isinstance(v, bool):
                raise RpcError(-32602, "Invalid params", f"select.{k} must be boolean")
        else:
            raise RpcError(-32602, "Invalid params", f"unknown select field {k!r}")
    return sel, limit, offset


def _sorted(ents: list[Entity], order: Order | None) -> list[Entity]:
    if order is None:
        return ents  # default: oldest first (creation sequence)
    ents = sorted(ents, key=lambda e: e.key)  # internal storage order breaks ties
    name, system = order.ref

    def val(e: Entity) -> Any:
        if system:
            return sort_value(order.type, {"$key": e.key, "$owner": e.owner, "$creator": e.creator, "$expiresAt": e.expires_at}[name])
        a = e.attr(name)  # unindexed attributes are sortable
        return None if a is None or a.type != order.type else sort_value(a.type, a.value)

    present = [(val(e), e) for e in ents]
    have = [p for p in present if p[0] is not None]
    missing = [p[1] for p in present if p[0] is None]
    have.sort(key=lambda p: p[0], reverse=order.desc)  # stable: ties keep $key order
    ordered = [p[1] for p in have]
    return ordered + missing if order.desc else missing + ordered


def project(e: Entity, sel: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    if sel.get("key", True):
        out["key"] = hx(e.key)
    full = e.to_wire()
    for f in ("owner", "creator", "createdAt", "updatedAt", "expiresAt", "creationFlags"):
        if sel.get(f):
            out[f] = full[f]
    attrs = sel.get("attributes")
    if attrs is True:
        out["attributes"] = full["attributes"]
    elif isinstance(attrs, dict):
        want = {k for k, v in attrs.items() if v}
        out["attributes"] = [a.to_wire() for a in e.attrs if a.name in want]
    if sel.get("attributeSchema"):
        out["attributeSchema"] = [{"name": a.name, "type": a.type, "indexed": a.indexed} for a in e.attrs]
    return out


def run_query(chain: Chain, params: Any, limits: Limits) -> dict[str, Any]:
    if not isinstance(params, list) or not 1 <= len(params) <= 2:
        raise RpcError(-32602, "Invalid params", "expected [query, options?]")
    if not isinstance(params[0], str):
        raise RpcError(-32602, "Invalid params", "query must be a string")
    sel, limit, offset = parse_options(params[1] if len(params) > 1 else None)
    q = parse_query(params[0], limits)
    matched = [e for e in chain.live_entities() if evaluate(q.cond, e)]
    matched = _sorted(matched, q.order)
    other_fields = any(v for k, v in sel.items() if k != "count")
    headless = bool(sel.get("count")) and not other_fields
    result: dict[str, Any] = {}
    if headless:
        result["data"] = []
    else:
        result["data"] = [project(e, sel) for e in matched[offset : offset + limit]]
    result["blockNumber"] = qty(chain.head)
    if sel.get("count"):
        result["count"] = qty(len(matched))
    return result


def get_entity(chain: Chain, params: Any) -> Any:
    if not isinstance(params, list) or len(params) != 1:
        raise RpcError(-32602, "Invalid params", "expected exactly one parameter: the 32-byte entity key")
    k = params[0]
    if not isinstance(k, str) or not re.fullmatch(r"0x[0-9a-fA-F]{64}", k):
        raise RpcError(-32602, "Invalid params", "entity key must be a 0x-prefixed 32-byte hex string")
    e = chain.get_live(bytes.fromhex(k[2:]))
    return e.to_wire() if e else None


__all__ = ["Limits", "run_query", "get_entity", "parse_query", "checksum_address", "wire_value"]
