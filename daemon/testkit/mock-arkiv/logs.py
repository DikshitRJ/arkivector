"""EVM log construction and ABI helpers for mock-arkiv.

Implements the five Arkiv events exactly as in
RESEARCH/arkiv-docs/03-evm-mutations-and-contract-spec/events-and-logs.md section 3,
plus the tiny subset of ABI encoding needed for `execute((uint8,bytes)[])` calldata
and `entityNonce(address)` eth_call results.

This module is the bottom of the dependency stack (keccak, hex and checksum
primitives live here) so state.py and query_lang.py may import it freely.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

from Crypto.Hash import keccak as _keccak

# --------------------------------------------------------------------------
# Primitives
# --------------------------------------------------------------------------


def keccak256(data: bytes) -> bytes:
    return _keccak.new(digest_bits=256, data=data).digest()


def hx(b: bytes) -> str:
    """Lowercase 0x-prefixed hex of raw bytes."""
    return "0x" + b.hex()


def qty(n: int) -> str:
    """Hex quantity (no leading zeros, 0 -> 0x0)."""
    if n < 0:
        raise ValueError("negative quantity")
    return hex(n)


def checksum_address(addr: bytes) -> str:
    """EIP-55 mixed-case checksum encoding of a 20-byte address."""
    if len(addr) != 20:
        raise ValueError("address must be 20 bytes")
    low = addr.hex()
    digest = keccak256(low.encode("ascii")).hex()
    out = "".join(c.upper() if c.isalpha() and int(digest[i], 16) >= 8 else c for i, c in enumerate(low))
    return "0x" + out


def is_valid_checksum(text: str) -> bool:
    """True if `text` (0x + 40 hex) is all-lower, all-upper, or a correct EIP-55 checksum."""
    body = text[2:]
    if body == body.lower() or body == body.upper():
        return True
    return checksum_address(bytes.fromhex(body)) == "0x" + body


# --------------------------------------------------------------------------
# Constants (verified against the doc at import time)
# --------------------------------------------------------------------------

DEFAULT_CONTRACT = "0x4400000000000000000000000000000000000044"

EVENT_SIGNATURES = {
    "EntityCreated": "EntityCreated(bytes32,address,uint64,uint8)",
    "EntityPatched": "EntityPatched(bytes32,address)",
    "ExpiryExtended": "ExpiryExtended(bytes32,address,uint64)",
    "OwnershipTransferred": "OwnershipTransferred(bytes32,address,address)",
    "EntityDeleted": "EntityDeleted(bytes32,address)",
}

# Documented topic0 constants (events-and-logs.md section 3.1 .. 3.5).
DOC_TOPIC0 = {
    "EntityCreated": "0xb282d7c494b8899aa8015cd07be621530beb03409eb8c5e8fdc1411ba64356a5",
    "EntityPatched": "0xe986ee220b2d7947a009d9389d8400b2ac82d3b430a9f72700a0975d3d5a34fe",
    "ExpiryExtended": "0x10dc3526654c9ba4e2370b1bfcbc0de087d141a5694b3d3305d37df7e6705d4b",
    "OwnershipTransferred": "0x0b659dccc8eb950324170e8d9598af5ee04ee070883eb28651a96788721fbf83",
    "EntityDeleted": "0x4059b76c47e1ecc40ba88e649b654f716515eb358c3ce83c803820e3b3130cc3",
}

TOPIC0 = {name: hx(keccak256(sig.encode("ascii"))) for name, sig in EVENT_SIGNATURES.items()}
for _name, _doc in DOC_TOPIC0.items():
    if TOPIC0[_name] != _doc:  # pragma: no cover - fails loudly at import
        raise AssertionError(f"topic0 mismatch for {_name}: computed {TOPIC0[_name]} doc {_doc}")

EXECUTE_SELECTOR = keccak256(b"execute((uint8,bytes)[])")[:4]  # 0x49650044 per execute-function.md
ENTITY_NONCE_SELECTOR = keccak256(b"entityNonce(address)")[:4]  # 0x36917bfd per overview.md 4.2
assert EXECUTE_SELECTOR.hex() == "49650044"
assert ENTITY_NONCE_SELECTOR.hex() == "36917bfd"

OP_CREATE, OP_PATCH, OP_EXTEND, OP_TRANSFER, OP_DELETE = 1, 2, 3, 4, 5
OP_CODES = {"create": 1, "patch": 2, "extend": 3, "transfer": 4, "delete": 5}


# --------------------------------------------------------------------------
# Log object
# --------------------------------------------------------------------------


@dataclass
class Log:
    address: str
    topics: list[str]
    data: str  # 0x-hex
    block_number: int
    block_hash: str
    tx_hash: str
    tx_index: int
    log_index: int
    removed: bool = False
    # op index inside its tx (informational for tests)
    op_index: int = field(default=0, compare=False)

    def to_rpc(self, removed: bool | None = None) -> dict[str, Any]:
        return {
            "address": self.address,
            "topics": list(self.topics),
            "data": self.data,
            "blockNumber": qty(self.block_number),
            "blockHash": self.block_hash,
            "transactionHash": self.tx_hash,
            "transactionIndex": qty(self.tx_index),
            "logIndex": qty(self.log_index),
            "removed": self.removed if removed is None else removed,
        }


def _word_u(n: int) -> bytes:
    return n.to_bytes(32, "big")


def _word_addr(a: bytes) -> bytes:
    return b"\x00" * 12 + a


def topic_addr(a: bytes) -> str:
    return hx(_word_addr(a))


# Each builder returns (topics, data_hex) per events-and-logs.md section 3.


def ev_entity_created(key: bytes, owner: bytes, expires_at: int, flags: int) -> tuple[list[str], str]:
    return (
        [TOPIC0["EntityCreated"], hx(key), topic_addr(owner)],
        hx(_word_u(expires_at) + _word_u(flags)),
    )


def ev_entity_patched(key: bytes, owner: bytes) -> tuple[list[str], str]:
    return [TOPIC0["EntityPatched"], hx(key), topic_addr(owner)], "0x"


def ev_expiry_extended(key: bytes, owner: bytes, expires_at: int) -> tuple[list[str], str]:
    return [TOPIC0["ExpiryExtended"], hx(key), topic_addr(owner)], hx(_word_u(expires_at))


def ev_ownership_transferred(key: bytes, prev_owner: bytes, new_owner: bytes) -> tuple[list[str], str]:
    return (
        [TOPIC0["OwnershipTransferred"], hx(key), topic_addr(prev_owner), topic_addr(new_owner)],
        "0x",
    )


def ev_entity_deleted(key: bytes, owner: bytes) -> tuple[list[str], str]:
    return [TOPIC0["EntityDeleted"], hx(key), topic_addr(owner)], "0x"


# --------------------------------------------------------------------------
# eth_getLogs / eth_subscribe("logs") filter matching
# --------------------------------------------------------------------------


class FilterError(ValueError):
    pass


def _norm_hex(v: Any, nbytes: int, what: str) -> str:
    if not isinstance(v, str) or not v.startswith("0x") or len(v) != 2 + 2 * nbytes:
        raise FilterError(f"{what} must be a {nbytes}-byte 0x-hex string")
    try:
        int(v[2:], 16)
    except ValueError as exc:
        raise FilterError(f"{what} is not valid hex") from exc
    return v.lower()


@dataclass(frozen=True)
class LogFilter:
    addresses: frozenset[str] | None  # None = any
    topics: tuple[frozenset[str] | None, ...]  # per position, None = wildcard

    @staticmethod
    def parse(address: Any, topics: Any) -> "LogFilter":
        addrs: frozenset[str] | None
        if address is None:
            addrs = None
        elif isinstance(address, str):
            addrs = frozenset([_norm_hex(address, 20, "address")])
        elif isinstance(address, list):
            addrs = frozenset(_norm_hex(a, 20, "address") for a in address)
        else:
            raise FilterError("address must be a string or array of strings")
        tops: list[frozenset[str] | None] = []
        if topics is None:
            topics = []
        if not isinstance(topics, list):
            raise FilterError("topics must be an array")
        if len(topics) > 4:
            raise FilterError("at most 4 topics supported")
        for t in topics:
            if t is None:
                tops.append(None)
            elif isinstance(t, str):
                tops.append(frozenset([_norm_hex(t, 32, "topic")]))
            elif isinstance(t, list):
                tops.append(frozenset(_norm_hex(x, 32, "topic") for x in t) if t else None)
            else:
                raise FilterError("topic must be null, a hash, or an array of hashes")
        return LogFilter(addrs, tuple(tops))

    def matches(self, log: Log) -> bool:
        if self.addresses is not None and log.address.lower() not in self.addresses:
            return False
        if len(self.topics) > len(log.topics):
            # filter wants more topics than the log has -> only OK if all extra are wildcards
            for t in self.topics[len(log.topics) :]:
                if t is not None:
                    return False
        for i, want in enumerate(self.topics):
            if want is None or i >= len(log.topics):
                continue
            if log.topics[i].lower() not in want:
                return False
        return True


# --------------------------------------------------------------------------
# Minimal ABI encoder (execute calldata)
# --------------------------------------------------------------------------


class Dyn:
    """Already-encoded dynamic member (placed in the tail of its tuple)."""

    def __init__(self, enc: bytes) -> None:
        self.enc = enc


def _pad32(b: bytes) -> bytes:
    return b + b"\x00" * (-len(b) % 32)


def enc_bytes(b: bytes) -> bytes:
    return _word_u(len(b)) + _pad32(b)


def enc_tuple(items: Sequence[bytes | Dyn]) -> bytes:
    head = b""
    tail = b""
    head_size = 32 * len(items)
    for it in items:
        if isinstance(it, Dyn):
            head += _word_u(head_size + len(tail))
            tail += it.enc
        else:
            assert len(it) == 32
            head += it
    return head + tail


def enc_array_dynamic(elems: Iterable[bytes]) -> bytes:
    """Array of dynamic elements: length, offsets, then element bodies."""
    elems = list(elems)
    head = b""
    tail = b""
    base = 32 * len(elems)
    for e in elems:
        head += _word_u(base + len(tail))
        tail += e
    return _word_u(len(elems)) + head + tail


def enc_execute(ops: Sequence[tuple[int, bytes]]) -> bytes:
    """Calldata for execute((uint8,bytes)[]) from (opcode, operationData) pairs."""
    elems = [enc_tuple([_word_u(code), Dyn(enc_bytes(data))]) for code, data in ops]
    return EXECUTE_SELECTOR + enc_tuple([Dyn(enc_array_dynamic(elems))])


def enc_attribute(name: str, type_id: int, value: bytes) -> bytes:
    name_b = name.encode("ascii").ljust(32, b"\x00")
    return enc_tuple([name_b, _word_u(type_id), Dyn(enc_bytes(value))])


def enc_create_data(salt: int, expires_at: int, min_lifetime: int, flags: int, attrs: list[bytes]) -> bytes:
    body = enc_tuple(
        [_word_u(salt), _word_u(expires_at), _word_u(min_lifetime), _word_u(flags), Dyn(enc_array_dynamic(attrs))]
    )
    return _word_u(32) + body  # abi.encode(struct) of a dynamic struct carries a leading offset


def enc_patch_data(key: bytes, attrs: list[bytes]) -> bytes:
    return _word_u(32) + enc_tuple([key, Dyn(enc_array_dynamic(attrs))])


def enc_extend_data(key: bytes, expires_at: int, min_lifetime: int) -> bytes:
    return key + _word_u(expires_at) + _word_u(min_lifetime)


def enc_transfer_data(key: bytes, new_owner: bytes) -> bytes:
    return key + _word_addr(new_owner)


def enc_delete_data(key: bytes) -> bytes:
    return key
