"""Chain and entity state for mock-arkiv.

Single-threaded by design (the aiohttp event loop is the only caller and no method awaits),
so every public method is atomic with respect to the loop.

Model
-----
* `Chain.blocks[n]` is the sealed block n (genesis = 0). `Chain.pending` is block head+1 and
  collects transactions until `seal_block()`.
* `Chain.working` is the mutable entity map (head + pending txs). `Chain.committed` is an immutable
  `Snapshot` of the state at the head; all reads (arkiv_query, arkiv_getEntity, eth_call) use it.
* `Entity` objects are immutable and replaced on every change, so a snapshot is just a shallow
  dict copy and history for reorgs is cheap.
* `Chain.apply_ops(sender, ops)` is the single mutation entry point (control API now, decoded
  `eth_sendRawTransaction` calldata in phase D5). A batch is atomic: any revert undoes the whole tx.

Determinism: block hash = sha256(parent || number || salt), salt = reorg epoch; timestamps are
genesis + 2n; entity keys follow entity-key-derivation.md; tx hashes derive from block hash,
tx index, sender and calldata. No wall clock or global RNG is used.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field, replace
from typing import Any, Iterator

from logs import (
    DEFAULT_CONTRACT,
    ENTITY_NONCE_SELECTOR,
    OP_CODES,
    Log,
    checksum_address,
    enc_attribute,
    enc_create_data,
    enc_delete_data,
    enc_execute,
    enc_extend_data,
    enc_patch_data,
    enc_transfer_data,
    ev_entity_created,
    ev_entity_deleted,
    ev_entity_patched,
    ev_expiry_extended,
    ev_ownership_transferred,
    hx,
    keccak256,
    qty,
)

# --------------------------------------------------------------------------
# Errors
# --------------------------------------------------------------------------


class RpcError(Exception):
    """JSON-RPC error (code/message/data) per error-handling.md."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.data = data

    def to_obj(self) -> dict[str, Any]:
        obj: dict[str, Any] = {"code": self.code, "message": self.message}
        if self.data is not None:
            obj["data"] = self.data
        return obj


class ControlError(Exception):
    """Invalid control-API input (HTTP 400)."""

    def __init__(self, message: str, details: Any = None) -> None:
        super().__init__(message)
        self.message = message
        self.details = details


class Revert(Exception):
    """EVM custom-error style revert of a whole tx (HTTP 409 from the control API)."""

    def __init__(self, error_name: str, /, **args: Any) -> None:
        super().__init__(error_name)
        self.name = error_name
        self.args_ = args

    def to_obj(self) -> dict[str, Any]:
        return {"error": self.name, "args": {k: _jsonable(v) for k, v in self.args_.items()}}


def _jsonable(v: Any) -> Any:
    if isinstance(v, (bytes, bytearray)):
        return hx(bytes(v))
    return v


# --------------------------------------------------------------------------
# Constants / limits (limits-and-quotas.md)
# --------------------------------------------------------------------------

GENESIS_TIMESTAMP = 1_700_000_000
BLOCK_DURATION = 2
MAX_LIFETIME = 7_884_000
MAX_STR_BYTES = 128
MAX_NAME_BYTES = 32
MAX_ATTRS = 32
MAX_BYTES_TOTAL = 131_072
DEC_SCALE = 10**18
U64_MAX = 2**64 - 1
U256_MAX = 2**256 - 1
I32_MIN, I32_MAX = -(2**31), 2**31 - 1
DEC_MIN, DEC_MAX = -(2**255), 2**255 - 1

TYPE_CODES = {
    "tombstone": 0,
    "bool": 1,
    "i32": 2,
    "u64": 3,
    "u256": 4,
    "dec": 5,
    "bytes": 7,
    "str": 8,
    "addr": 9,
    "key": 10,
}
VALUE_TYPES = ("bool", "i32", "u64", "u256", "dec", "str", "addr", "key", "bytes")

KEYWORDS = {
    "and", "or", "not", "true", "false", "between", "startswith", "exists", "typeof",
    "order", "by", "asc", "desc",
    # additional names reserved by attribute-serialization.md 3.2
    "select", "from", "where", "limit", "offset", "like", "is", "null",
}  # fmt: skip
RESERVED_NAMES = KEYWORDS | set(VALUE_TYPES)
NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9._-]{0,31}$")
_HEX_RE = re.compile(r"^0x[0-9a-fA-F]*$")
_DEC_RE = re.compile(r"^(-?)(\d+)(?:\.(\d+))?$")


def validate_attr_name(name: Any) -> str:
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise ControlError(f"invalid attribute name {name!r}: must match [A-Za-z][A-Za-z0-9._-]{{0,31}}")
    if "--" in name:
        raise ControlError(f"invalid attribute name {name!r}: '--' is reserved for comments")
    if name.lower() in RESERVED_NAMES:
        raise ControlError(f"invalid attribute name {name!r}: reserved word")
    return name


# --------------------------------------------------------------------------
# Typed values
# --------------------------------------------------------------------------


def parse_hex_bytes(v: Any, nbytes: int | None, what: str) -> bytes:
    if not isinstance(v, str) or not _HEX_RE.match(v):
        raise ValueError(f"{what} must be a 0x-prefixed hex string")
    body = v[2:]
    if len(body) % 2:
        raise ValueError(f"{what} has odd hex length")
    raw = bytes.fromhex(body)
    if nbytes is not None and len(raw) != nbytes:
        raise ValueError(f"{what} must be {nbytes} bytes, got {len(raw)}")
    return raw


def _parse_int(v: Any, lo: int, hi: int, what: str) -> int:
    if isinstance(v, bool):
        raise ValueError(f"{what} must be an integer")
    if isinstance(v, int):
        n = v
    elif isinstance(v, str):
        try:
            n = int(v, 16) if v.lower().startswith(("0x", "-0x")) else int(v, 10)
        except ValueError as exc:
            raise ValueError(f"{what} is not an integer: {v!r}") from exc
    else:
        raise ValueError(f"{what} must be an integer or numeric string")
    if not lo <= n <= hi:
        raise ValueError(f"{what} out of range [{lo}, {hi}]")
    return n


def parse_dec(v: Any) -> int:
    """Decimal text/number -> value scaled by 1e18 (exact; > 18 fractional digits is an error)."""
    if isinstance(v, bool):
        raise ValueError("dec must be a number or numeric string")
    if isinstance(v, float):
        v = repr(v)
    elif isinstance(v, int):
        v = str(v)
    if not isinstance(v, str):
        raise ValueError("dec must be a number or numeric string")
    m = _DEC_RE.match(v.strip())
    if not m:
        raise ValueError(f"invalid decimal {v!r}")
    sign, whole, frac = m.group(1), m.group(2), m.group(3) or ""
    if len(frac) > 18:
        raise ValueError("dec supports at most 18 fractional digits")
    n = int(whole) * DEC_SCALE + int(frac.ljust(18, "0") or "0")
    if sign:
        n = -n
    if not DEC_MIN <= n <= DEC_MAX:
        raise ValueError("dec out of signed 256-bit range")
    return n


def parse_value(typ: str, v: Any) -> Any:
    """Wire/JSON value -> internal value. Raises ValueError with a human message."""
    if typ == "bool":
        if not isinstance(v, bool):
            raise ValueError("bool must be true or false")
        return v
    if typ == "i32":
        return _parse_int(v, I32_MIN, I32_MAX, "i32")
    if typ == "u64":
        return _parse_int(v, 0, U64_MAX, "u64")
    if typ == "u256":
        return _parse_int(v, 0, U256_MAX, "u256")
    if typ == "dec":
        return parse_dec(v)
    if typ == "str":
        if not isinstance(v, str):
            raise ValueError("str must be a string")
        if len(v.encode("utf-8")) > MAX_STR_BYTES:
            raise ValueError(f"str exceeds {MAX_STR_BYTES} UTF-8 bytes")
        return v
    if typ == "addr":
        return parse_hex_bytes(v, 20, "addr")
    if typ == "key":
        return parse_hex_bytes(v, 32, "key")
    if typ == "bytes":
        raw = parse_hex_bytes(v, None, "bytes")
        if len(raw) > MAX_BYTES_TOTAL:
            raise ValueError("bytes value exceeds 128 KiB")
        return raw
    raise ValueError(f"unknown attribute type {typ!r}")


def format_dec(n: int, trim: bool = False) -> str:
    sign = "-" if n < 0 else ""
    a = abs(n)
    whole, frac = divmod(a, DEC_SCALE)
    fs = f"{frac:018d}"
    if trim:
        fs = fs.rstrip("0")
        return f"{sign}{whole}" + (f".{fs}" if fs else "")
    return f"{sign}{whole}.{fs}"


DEC_TRIM = False  # set by server --dec-trim; wire format of `dec` (see README)


def wire_value(typ: str, v: Any) -> Any:
    """Internal value -> JSON per arkiv_query.md 4.2."""
    if typ in ("bool", "i32", "str"):
        return v
    if typ in ("u64", "u256"):
        return qty(v)
    if typ == "dec":
        return format_dec(v, DEC_TRIM)
    if typ == "addr":
        return checksum_address(v)
    if typ in ("key", "bytes"):
        return hx(v)
    raise ValueError(typ)


def abi_value(typ: str, v: Any) -> bytes:
    """Internal value -> Solidity `bytes value` payload (attribute-serialization.md 5)."""
    if typ == "bool":
        return (1 if v else 0).to_bytes(32, "big")
    if typ == "i32":
        return (v % 2**256).to_bytes(32, "big")
    if typ in ("u64", "u256"):
        return v.to_bytes(32, "big")
    if typ == "dec":
        return (v % 2**256).to_bytes(32, "big")
    if typ == "addr":
        return b"\x00" * 12 + v
    if typ == "key":
        return v
    if typ == "str":
        return v.encode("utf-8")
    if typ == "bytes":
        return v
    if typ == "tombstone":
        return b""
    raise ValueError(typ)


def sort_value(typ: str, v: Any) -> Any:
    """Comparable form: str compares by UTF-8 bytes (no collation); everything else natively."""
    if typ == "str":
        return v.encode("utf-8")
    if typ == "bool":
        return int(v)
    return v


# --------------------------------------------------------------------------
# Attributes / entities
# --------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Attr:
    name: str
    type: str  # one of VALUE_TYPES, or "tombstone" (patch only)
    indexed: bool
    value: Any

    def to_wire(self) -> dict[str, Any]:
        return {"name": self.name, "type": self.type, "value": wire_value(self.type, self.value)}

    def abi(self) -> bytes:
        tid = TYPE_CODES[self.type] | (0x80 if self.indexed else 0)
        return enc_attribute(self.name, tid, abi_value(self.type, self.value))


def parse_attr(raw: Any, allow_tombstone: bool) -> Attr:
    if not isinstance(raw, dict):
        raise ControlError("attribute must be an object {name,type,value[,indexed]}")
    unknown = set(raw) - {"name", "type", "value", "indexed"}
    if unknown:
        raise ControlError(f"unknown attribute fields {sorted(unknown)}")
    name = validate_attr_name(raw.get("name"))
    typ = raw.get("type")
    if typ == "tombstone":
        if not allow_tombstone:
            raise Revert("TombstoneInCreate", name=name)
        return Attr(name, "tombstone", False, None)
    if typ not in VALUE_TYPES:
        raise ControlError(f"attribute {name!r}: type must be one of {list(VALUE_TYPES)} (or 'tombstone' in patch)")
    if "value" not in raw:
        raise ControlError(f"attribute {name!r}: missing value")
    try:
        value = parse_value(typ, raw["value"])
    except ValueError as exc:
        raise ControlError(f"attribute {name!r}: {exc}") from exc
    indexed = raw.get("indexed", typ != "bytes")
    if not isinstance(indexed, bool):
        raise ControlError(f"attribute {name!r}: indexed must be boolean")
    if indexed and typ == "bytes":
        raise Revert("InvalidValueType", name=name, typeId=0x87)
    return Attr(name, typ, indexed, value)


def parse_attrs(raw: Any, allow_tombstone: bool) -> tuple[Attr, ...]:
    if raw is None:
        return ()
    if not isinstance(raw, list):
        raise ControlError("attributes must be an array")
    out: dict[str, Attr] = {}
    total_bytes = 0
    for item in raw:
        a = parse_attr(item, allow_tombstone)
        if a.name in out:
            raise ControlError(f"duplicate attribute {a.name!r}")
        out[a.name] = a
        if a.type == "bytes":
            total_bytes += len(a.value)
    if total_bytes > MAX_BYTES_TOTAL:
        raise Revert("EntityTooLargeError", bytes=total_bytes, max=MAX_BYTES_TOTAL)
    # The contract demands strictly ascending byte order; the control API accepts any order.
    return tuple(sorted(out.values(), key=lambda a: a.name.encode("ascii")))


@dataclass(frozen=True, slots=True)
class Entity:
    key: bytes
    owner: bytes
    creator: bytes
    created_at: int
    updated_at: int
    expires_at: int
    flags: int
    attrs: tuple[Attr, ...]  # sorted by name
    seq: int  # creation sequence ("oldest first" default order)

    def attr(self, name: str) -> Attr | None:
        for a in self.attrs:
            if a.name == name:
                return a
        return None

    def creation_flags(self) -> dict[str, bool]:
        return {
            "readonly": bool(self.flags & 1),
            "permissionlessExtension": bool(self.flags & 2),
            "raw": False,
        }

    def to_wire(self) -> dict[str, Any]:
        """Full entity as returned by arkiv_getEntity."""
        return {
            "key": hx(self.key),
            "owner": checksum_address(self.owner),
            "creator": checksum_address(self.creator),
            "createdAt": qty(self.created_at),
            "updatedAt": qty(self.updated_at),
            "expiresAt": qty(self.expires_at),
            "creationFlags": self.creation_flags(),
            "attributes": [a.to_wire() for a in self.attrs],
        }


# --------------------------------------------------------------------------
# Operations (internal form shared by control API and, later, tx decoding)
# --------------------------------------------------------------------------


@dataclass
class Op:
    kind: str  # create | patch | extend | transfer | delete
    key: bytes | None = None  # target (patch/extend/transfer/delete) or preset key (create)
    salt: int = 0
    expires_at: int = 0
    min_lifetime: int = 0
    flags: int = 0
    attrs: tuple[Attr, ...] = ()
    new_owner: bytes | None = None
    creator: bytes | None = None  # create with preset key only
    unchecked: bool = False  # skip MAX_LIFETIME check (preset/seed data)

    def abi(self) -> tuple[int, bytes]:
        code = OP_CODES[self.kind]
        if self.kind == "create":
            return code, enc_create_data(
                self.salt, self.expires_at, self.min_lifetime, self.flags, [a.abi() for a in self.attrs]
            )
        assert self.key is not None
        if self.kind == "patch":
            return code, enc_patch_data(self.key, [a.abi() for a in self.attrs])
        if self.kind == "extend":
            return code, enc_extend_data(self.key, self.expires_at, self.min_lifetime)
        if self.kind == "transfer":
            assert self.new_owner is not None
            return code, enc_transfer_data(self.key, self.new_owner)
        return code, enc_delete_data(self.key)


DEFAULT_MIN_LIFETIME = 1_000_000
DEFAULT_SENDER = bytes.fromhex("11" * 20)


def parse_address(v: Any, what: str) -> bytes:
    try:
        return parse_hex_bytes(v, 20, what)
    except ValueError as exc:
        raise ControlError(str(exc)) from exc


def parse_key(v: Any, what: str = "key") -> bytes:
    try:
        return parse_hex_bytes(v, 32, what)
    except ValueError as exc:
        raise ControlError(str(exc)) from exc


def _uint(raw: dict[str, Any], field_: str, hi: int, default: int = 0) -> int:
    if field_ not in raw:
        return default
    try:
        return _parse_int(raw[field_], 0, hi, field_)
    except ValueError as exc:
        raise ControlError(str(exc)) from exc


_OP_FIELDS = {
    "create": {"op", "key", "salt", "expires_at", "min_lifetime", "flags", "attributes", "creator", "unchecked"},
    "patch": {"op", "key", "attributes"},
    "extend": {"op", "key", "expires_at", "min_lifetime"},
    "transfer": {"op", "key", "new_owner"},
    "delete": {"op", "key"},
}


def parse_op(raw: Any, kind: str | None = None) -> Op:
    """Validate one control-API op object. `kind` may be implied by the endpoint."""
    if not isinstance(raw, dict):
        raise ControlError("op must be a JSON object")
    kind = kind or raw.get("op")
    if kind not in _OP_FIELDS:
        raise ControlError(f"op must be one of {sorted(_OP_FIELDS)}")
    extra = set(raw) - _OP_FIELDS[kind] - {"owner", "sender"}
    if extra:
        raise ControlError(f"unknown fields for {kind}: {sorted(extra)}")
    if kind == "create":
        has_exp = "expires_at" in raw or "min_lifetime" in raw or "ttl" in raw
        op = Op(
            "create",
            salt=_uint(raw, "salt", 2**128 - 1),
            expires_at=_uint(raw, "expires_at", U64_MAX),
            min_lifetime=_uint(raw, "min_lifetime", U64_MAX),
            flags=_uint(raw, "flags", 255),
            attrs=parse_attrs(raw.get("attributes"), allow_tombstone=False),
            unchecked=bool(raw.get("unchecked", False)),
        )
        if not has_exp:
            op.min_lifetime = DEFAULT_MIN_LIFETIME
        if "key" in raw and raw["key"] is not None:
            op.key = parse_key(raw["key"])
            op.unchecked = True  # preset keys (seed data) may carry any expiry
        if raw.get("creator") is not None:
            if op.key is None:
                raise ControlError("creator may only be preset together with a preset key")
            op.creator = parse_address(raw["creator"], "creator")
        return op
    if "key" not in raw:
        raise ControlError(f"{kind} requires 'key'")
    op = Op(kind, key=parse_key(raw["key"]))
    if kind == "patch":
        op.attrs = parse_attrs(raw.get("attributes"), allow_tombstone=True)
    elif kind == "extend":
        op.expires_at = _uint(raw, "expires_at", U64_MAX)
        op.min_lifetime = _uint(raw, "min_lifetime", U64_MAX)
    elif kind == "transfer":
        if "new_owner" not in raw:
            raise ControlError("transfer requires 'new_owner'")
        op.new_owner = parse_address(raw["new_owner"], "new_owner")
    return op


# --------------------------------------------------------------------------
# Blocks / transactions
# --------------------------------------------------------------------------


@dataclass
class Tx:
    hash: bytes
    index: int
    sender: bytes
    to: bytes
    input: bytes
    nonce: int
    logs: list[Log]
    block_number: int
    block_hash: bytes

    def to_rpc(self, pending: bool = False) -> dict[str, Any]:
        return {
            "hash": hx(self.hash),
            "nonce": qty(self.nonce),
            "blockHash": None if pending else hx(self.block_hash),
            "blockNumber": None if pending else qty(self.block_number),
            "transactionIndex": None if pending else qty(self.index),
            "from": checksum_address(self.sender),
            "to": checksum_address(self.to),
            "value": "0x0",
            "gas": "0x1c9c380",
            "gasPrice": "0x3b9aca00",
            "maxFeePerGas": "0x3b9aca00",
            "maxPriorityFeePerGas": "0x0",
            "input": hx(self.input),
            "type": "0x2",
            "chainId": None,  # filled by server (needs chain id)
            "accessList": [],
            "v": "0x0",
            "r": "0x0",
            "s": "0x0",
        }

    def receipt(self) -> dict[str, Any]:
        gas = 21000 + 16 * len(self.input)
        return {
            "transactionHash": hx(self.hash),
            "transactionIndex": qty(self.index),
            "blockHash": hx(self.block_hash),
            "blockNumber": qty(self.block_number),
            "from": checksum_address(self.sender),
            "to": checksum_address(self.to),
            "cumulativeGasUsed": qty(gas),
            "gasUsed": qty(gas),
            "effectiveGasPrice": "0x3b9aca00",
            "contractAddress": None,
            "logs": [lg.to_rpc() for lg in self.logs],
            "logsBloom": "0x" + "00" * 256,
            "status": "0x1",
            "type": "0x2",
        }


@dataclass
class Block:
    number: int
    hash: bytes
    parent_hash: bytes
    timestamp: int
    txs: list[Tx] = field(default_factory=list)
    log_count: int = 0

    def to_rpc(self, full: bool, chain_id: int) -> dict[str, Any]:
        if full:
            txs: list[Any] = []
            for t in self.txs:
                o = t.to_rpc()
                o["chainId"] = qty(chain_id)
                txs.append(o)
        else:
            txs = [hx(t.hash) for t in self.txs]
        gas = sum(21000 + 16 * len(t.input) for t in self.txs)
        zero32 = "0x" + "00" * 32
        return {
            "number": qty(self.number),
            "hash": hx(self.hash),
            "parentHash": hx(self.parent_hash),
            "nonce": "0x0000000000000000",
            "sha3Uncles": "0x1dcc4de8dec75d7aab85b567b6ccd41ad312451b948a7413f0a142fd40d49347",
            "logsBloom": "0x" + "00" * 256,
            "transactionsRoot": zero32,
            "stateRoot": zero32,
            "receiptsRoot": zero32,
            "miner": "0x" + "00" * 20,
            "difficulty": "0x0",
            "totalDifficulty": "0x0",
            "extraData": "0x",
            "size": qty(512 + sum(len(t.input) for t in self.txs)),
            "gasLimit": "0x1c9c380",
            "gasUsed": qty(gas),
            "timestamp": qty(self.timestamp),
            "transactions": txs,
            "uncles": [],
            "baseFeePerGas": "0x3b9aca00",
            "mixHash": zero32,
        }


@dataclass(frozen=True)
class Snapshot:
    """Immutable view of chain state at the end of a block."""

    entities: dict[bytes, Entity]
    nonces: dict[bytes, int]  # Arkiv entityNonce per owner
    acct_nonces: dict[bytes, int]  # account tx count per sender
    seq: int


# --------------------------------------------------------------------------
# Chain
# --------------------------------------------------------------------------


def derive_key(chain_id: int, contract: bytes, owner: bytes, nonce: int, salt: int) -> bytes:
    """keccak256(abi.encodePacked(chainId u256, arkiv addr, owner addr, nonce u64, salt u128))."""
    packed = chain_id.to_bytes(32, "big") + contract + owner + nonce.to_bytes(8, "big") + salt.to_bytes(16, "big")
    return keccak256(packed)


class Chain:
    def __init__(
        self,
        seed: int,
        chain_id: int = 0x76EDAF,
        contract: str = DEFAULT_CONTRACT,
        max_history: int = 256,
        max_lifetime: int = MAX_LIFETIME,
    ) -> None:
        self.chain_id = chain_id
        self.contract = bytes.fromhex(contract[2:])
        self.max_history = max_history
        self.max_lifetime = max_lifetime
        self.reset(seed)

    # ---- lifecycle ----------------------------------------------------

    def reset(self, seed: int | None = None) -> None:
        if seed is not None:
            self.seed = seed
        self.epoch = 0
        genesis_hash = hashlib.sha256(b"mock-arkiv-genesis" + self.seed.to_bytes(8, "big", signed=False)).digest()
        genesis = Block(0, genesis_hash, b"\x00" * 32, GENESIS_TIMESTAMP)
        self.blocks: list[Block] = [genesis]
        self.committed = Snapshot({}, {}, {}, 0)
        self.working: dict[bytes, Entity] = {}
        self.nonces: dict[bytes, int] = {}
        self.acct_nonces: dict[bytes, int] = {}
        self.seq = 0
        self.history: dict[int, Snapshot] = {0: self.committed}
        self.tx_index: dict[bytes, Tx] = {}
        self.pending = self._new_pending()

    @property
    def head(self) -> int:
        return len(self.blocks) - 1

    @property
    def head_block(self) -> Block:
        return self.blocks[-1]

    def _new_pending(self) -> Block:
        parent = self.blocks[-1]
        n = parent.number + 1
        h = hashlib.sha256(parent.hash + n.to_bytes(8, "big") + self.epoch.to_bytes(8, "big")).digest()
        return Block(n, h, parent.hash, GENESIS_TIMESTAMP + BLOCK_DURATION * n)

    # ---- reads ----------------------------------------------------------

    def live_entities(self) -> list[Entity]:
        """Entities live at the head, in creation order (default arkiv_query order)."""
        head = self.head
        ents = [e for e in self.committed.entities.values() if e.expires_at > head]
        ents.sort(key=lambda e: e.seq)
        return ents

    def get_live(self, key: bytes) -> Entity | None:
        e = self.committed.entities.get(key)
        if e is not None and e.expires_at > self.head:
            return e
        return None

    def entity_nonce(self, owner: bytes) -> int:
        return self.committed.nonces.get(owner, 0)

    def block_by_number(self, n: int) -> Block | None:
        if 0 <= n < len(self.blocks):
            return self.blocks[n]
        return None

    def block_by_hash(self, h: bytes) -> Block | None:
        for b in reversed(self.blocks):
            if b.hash == h:
                return b
        return None

    def logs_in_range(self, lo: int, hi: int) -> Iterator[Log]:
        for n in range(max(lo, 0), min(hi, self.head) + 1):
            for tx in self.blocks[n].txs:
                yield from tx.logs

    # ---- mutation -----------------------------------------------------

    def resolve_sender(self, sender: bytes | None, ops: list[Op]) -> bytes:
        """Sender for a tx: explicit, else the current owner of the first op's target entity."""
        if sender is not None:
            return sender
        first = ops[0]
        if first.key is not None and first.kind != "create":
            e = self.working.get(first.key)
            if e is not None:
                return e.owner
        return DEFAULT_SENDER

    def apply_ops(
        self,
        sender: bytes,
        ops: list[Op],
        *,
        tx_hash: bytes | None = None,
        calldata: bytes | None = None,
    ) -> list[Log]:
        """Apply one `execute(ops)` tx from `sender` to the pending block, atomically.

        Returns the logs (one per op, op order). Raises Revert (state untouched) on any failure.
        D5 reuses this for decoded eth_sendRawTransaction calldata (pass tx_hash/calldata).
        """
        if not ops:
            raise Revert("EmptyBatch")
        pend = self.pending
        cur = pend.number
        if calldata is None:
            calldata = enc_execute([op.abi() for op in ops])
        idx = len(pend.txs)
        if tx_hash is None:
            tx_hash = keccak256(pend.hash + idx.to_bytes(4, "big") + sender + calldata)
        undo: list[tuple[str, Any, Any]] = []
        logs: list[Log] = []
        seq0 = self.seq
        try:
            for i, op in enumerate(ops):
                topics, data = self._apply_one(sender, op, cur, undo)
                logs.append(
                    Log(
                        address=hx(self.contract),
                        topics=topics,
                        data=data,
                        block_number=cur,
                        block_hash=hx(pend.hash),
                        tx_hash=hx(tx_hash),
                        tx_index=idx,
                        log_index=pend.log_count + i,
                        op_index=i,
                    )
                )
            tx_nonce = self.acct_nonces.get(sender, 0)
            undo.append(("a", sender, self.acct_nonces.get(sender)))
            self.acct_nonces[sender] = tx_nonce + 1
        except Exception:
            for kind, k, old in reversed(undo):
                table = {"e": self.working, "n": self.nonces, "a": self.acct_nonces}[kind]
                if old is None:
                    table.pop(k, None)
                else:
                    table[k] = old
            self.seq = seq0
            raise
        tx = Tx(tx_hash, idx, sender, self.contract, calldata, tx_nonce, logs, cur, pend.hash)
        pend.txs.append(tx)
        pend.log_count += len(logs)
        self.tx_index[tx_hash] = tx
        return logs

    def _live(self, key: bytes, cur: int) -> Entity:
        e = self.working.get(key)
        if e is None:
            raise Revert("EntityNotFound", entityKey=key)
        if e.expires_at <= cur:
            raise Revert("EntityExpired", entityKey=key, expiresAt=e.expires_at)
        return e

    def _put(self, e: Entity, undo: list[tuple[str, Any, Any]]) -> None:
        undo.append(("e", e.key, self.working.get(e.key)))
        self.working[e.key] = e

    def _resolve_expiry(self, op: Op, cur: int) -> int:
        expiry = max(op.expires_at, cur + op.min_lifetime)
        if expiry <= cur:
            raise Revert("ExpiryDeadOnArrival", target=expiry, currentBlock=cur)
        if expiry > cur + self.max_lifetime and not op.unchecked:
            raise Revert("ExpiryBeyondMaxLifetime", target=expiry, currentBlock=cur, maxLifetime=self.max_lifetime)
        return expiry

    def _apply_one(
        self, sender: bytes, op: Op, cur: int, undo: list[tuple[str, Any, Any]]
    ) -> tuple[list[str], str]:
        if op.kind == "create":
            if op.flags & ~0x03:
                raise Revert("ReservedCreationFlags", creationFlags=op.flags)
            if any(a.type == "tombstone" for a in op.attrs):
                raise Revert("TombstoneInCreate")
            if len(op.attrs) > MAX_ATTRS:
                raise Revert("TooManyAttributes", count=len(op.attrs), max=MAX_ATTRS)
            expiry = self._resolve_expiry(op, cur)
            if op.key is not None:
                key = op.key
                if key in self.working:
                    raise Revert("EntityKeyCollision", entityKey=key)
            else:
                nonce = self.nonces.get(sender, 0)
                key = derive_key(self.chain_id, self.contract, sender, nonce, op.salt)
                if key in self.working:
                    raise Revert("EntityKeyCollision", entityKey=key)
                undo.append(("n", sender, self.nonces.get(sender)))
                self.nonces[sender] = nonce + 1
            ent = Entity(
                key=key,
                owner=sender,
                creator=op.creator or sender,
                created_at=cur,
                updated_at=cur,
                expires_at=expiry,
                flags=op.flags,
                attrs=op.attrs,
                seq=self.seq,
            )
            self.seq += 1
            self._put(ent, undo)
            return ev_entity_created(key, sender, expiry, op.flags)

        assert op.key is not None
        ent = self._live(op.key, cur)
        if op.kind == "extend":
            if sender != ent.owner and not ent.flags & 0x02:
                raise Revert("NotOwner", entityKey=ent.key, caller=sender, owner=ent.owner)
            new_expiry = max(op.expires_at, cur + op.min_lifetime)
            if new_expiry < ent.expires_at:
                raise Revert("ExpiryNotExtended", entityKey=ent.key, newExpiresAt=new_expiry, currentExpiresAt=ent.expires_at)
            if new_expiry > cur + self.max_lifetime and not op.unchecked:
                raise Revert("ExpiryBeyondMaxLifetime", target=new_expiry, currentBlock=cur, maxLifetime=self.max_lifetime)
            if new_expiry != ent.expires_at:
                self._put(replace(ent, expires_at=new_expiry, updated_at=cur), undo)
            return ev_expiry_extended(ent.key, ent.owner, new_expiry)

        if sender != ent.owner:
            raise Revert("NotOwner", entityKey=ent.key, caller=sender, owner=ent.owner)

        if op.kind == "patch":
            if ent.flags & 0x01:
                raise Revert("ReadOnlyEntity", entityKey=ent.key)
            if not op.attrs:
                raise Revert("EmptyMutations", entityKey=ent.key)
            merged = {a.name: a for a in ent.attrs}
            for m in op.attrs:
                if m.type == "tombstone":
                    merged.pop(m.name, None)
                else:
                    merged[m.name] = m
            attrs = tuple(sorted(merged.values(), key=lambda a: a.name.encode("ascii")))
            self._put(replace(ent, attrs=attrs, updated_at=cur), undo)
            return ev_entity_patched(ent.key, ent.owner)

        if op.kind == "transfer":
            assert op.new_owner is not None
            if op.new_owner == b"\x00" * 20:
                raise Revert("TransferToZeroAddress", entityKey=ent.key)
            if op.new_owner == ent.owner:
                raise Revert("TransferToSelf", entityKey=ent.key)
            self._put(replace(ent, owner=op.new_owner), undo)  # creator and updated_at unchanged
            return ev_ownership_transferred(ent.key, ent.owner, op.new_owner)

        if op.kind == "delete":
            undo.append(("e", ent.key, ent))
            del self.working[ent.key]
            return ev_entity_deleted(ent.key, ent.owner)

        raise AssertionError(op.kind)

    # ---- mining ---------------------------------------------------------

    def seal_block(self) -> Block:
        blk = self.pending
        if blk.txs:
            self.committed = Snapshot(dict(self.working), dict(self.nonces), dict(self.acct_nonces), self.seq)
        self.blocks.append(blk)
        self.history[blk.number] = self.committed
        old = blk.number - self.max_history
        if old in self.history and old != 0:
            del self.history[old]
        self.pending = self._new_pending()
        return blk

    def mine(self, n: int) -> list[Block]:
        return [self.seal_block() for _ in range(n)]

    def reorg(
        self, depth: int, replacement: list[tuple[bytes, list[Op]]], extra_blocks: int = 0
    ) -> tuple[list[Block], list[Block]]:
        """Rewind `depth` blocks, then mine depth+extra_blocks new blocks (replacement txs in the first).

        Returns (removed_blocks, new_blocks). Any pending txs are discarded. On failure of a
        replacement tx the chain is restored exactly and the Revert propagates.
        """
        head = self.head
        if depth < 1 or depth > head:
            raise ControlError(f"depth must be in [1, {head}]")
        target = head - depth
        if target not in self.history:
            raise ControlError(f"reorg depth {depth} exceeds retained history ({self.max_history} blocks)")
        saved = (
            list(self.blocks), self.committed, dict(self.history), self.epoch, self.pending,
            dict(self.working), dict(self.nonces), dict(self.acct_nonces), self.seq, dict(self.tx_index),
        )  # fmt: skip
        removed = self.blocks[target + 1 :]
        try:
            for b in removed + [self.pending]:
                for t in b.txs:
                    self.tx_index.pop(t.hash, None)
            self.blocks = self.blocks[: target + 1]
            for n in [n for n in self.history if n > target]:
                del self.history[n]
            snap = self.history[target]
            self.committed = snap
            self.working = dict(snap.entities)
            self.nonces = dict(snap.nonces)
            self.acct_nonces = dict(snap.acct_nonces)
            self.seq = snap.seq
            self.epoch += 1
            self.pending = self._new_pending()
            new_blocks: list[Block] = []
            for i in range(depth + extra_blocks):
                if i == 0:
                    for sender, ops in replacement:
                        self.apply_ops(self.resolve_sender(sender, ops), ops)
                new_blocks.append(self.seal_block())
        except Exception:
            (self.blocks, self.committed, self.history, self.epoch, self.pending,
             self.working, self.nonces, self.acct_nonces, self.seq, self.tx_index) = saved  # fmt: skip
            raise
        return removed, new_blocks

    # ---- eth_call -------------------------------------------------------

    def eth_call(self, to: bytes, data: bytes) -> bytes:
        if to != self.contract:
            raise RpcError(-32000, "execution reverted: no contract at address")
        if data[:4] == ENTITY_NONCE_SELECTOR and len(data) == 36:
            return self.entity_nonce(data[16:36]).to_bytes(32, "big")
        raise RpcError(3, "execution reverted", "0x")
