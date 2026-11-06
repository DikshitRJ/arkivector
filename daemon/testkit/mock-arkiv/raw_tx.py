"""EIP-1559 and legacy raw transaction decoding and execution for mock-arkiv."""

from __future__ import annotations

from typing import Any
from Crypto.Hash import keccak as _keccak

from logs import EXECUTE_SELECTOR, hx, keccak256
from state import Attr, Chain, Op, Revert, RpcError

# secp256k1 curve parameters
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
GX = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
GY = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8


def pt_add(p1: tuple[int, int] | None, p2: tuple[int, int] | None) -> tuple[int, int] | None:
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        l = (3 * x1 * x1 % P) * pow(2 * y1, -1, P) % P
    else:
        l = ((y2 - y1) % P) * pow((x2 - x1) % P, -1, P) % P
    x3 = (l * l - x1 - x2) % P
    y3 = (l * (x1 - x3) - y1) % P
    return (x3 % P, y3 % P)


def pt_mul(k: int, pt: tuple[int, int]) -> tuple[int, int] | None:
    k = k % N
    res: tuple[int, int] | None = None
    addend: tuple[int, int] | None = pt
    while k:
        if k & 1:
            res = pt_add(res, addend)
        addend = pt_add(addend, addend)
        k >>= 1
    return res


def recover_sender(sighash: bytes, r: int, s: int, v_parity: int) -> bytes:
    e = int.from_bytes(sighash, "big")
    x = r
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if (y % 2) != (v_parity % 2):
        y = P - y
    r_pt = (x, y)
    s_r = pt_mul(s, r_pt)
    e_g = pt_mul(e, (GX, GY))
    if e_g is None or s_r is None:
        raise ValueError("point multiplication failed")
    neg_e_g = (e_g[0], (P - e_g[1]) % P)
    diff = pt_add(s_r, neg_e_g)
    if diff is None:
        raise ValueError("point addition failed")
    q = pt_mul(pow(r, -1, N), diff)
    if q is None:
        raise ValueError("public key recovery failed")
    pub = q[0].to_bytes(32, "big") + q[1].to_bytes(32, "big")
    k = _keccak.new(digest_bits=256, data=pub)
    return k.digest()[-20:]


def rlp_decode(b: bytes, pos: int = 0) -> tuple[Any, int]:
    if pos >= len(b):
        raise ValueError("unexpected end of RLP data")
    prefix = b[pos]
    if prefix < 0x80:
        return b[pos : pos + 1], pos + 1
    if prefix <= 0xB7:
        length = prefix - 0x80
        return b[pos + 1 : pos + 1 + length], pos + 1 + length
    if prefix <= 0xBF:
        ll = prefix - 0xB7
        length = int.from_bytes(b[pos + 1 : pos + 1 + ll], "big")
        start = pos + 1 + ll
        return b[start : start + length], start + length
    if prefix <= 0xF7:
        length = prefix - 0xC0
        end = pos + 1 + length
        items = []
        cur = pos + 1
        while cur < end:
            it, cur = rlp_decode(b, cur)
            items.append(it)
        return items, end
    ll = prefix - 0xF7
    length = int.from_bytes(b[pos + 1 : pos + 1 + ll], "big")
    start = pos + 1 + ll
    end = start + length
    items = []
    cur = start
    while cur < end:
        it, cur = rlp_decode(b, cur)
        items.append(it)
    return items, end


def rlp_encode(val: Any) -> bytes:
    if isinstance(val, (bytes, bytearray)):
        b = bytes(val)
        if len(b) == 1 and b[0] < 0x80:
            return b
        if len(b) <= 55:
            return bytes([0x80 + len(b)]) + b
        lb = len(b).to_bytes((len(b).bit_length() + 7) // 8, "big")
        return bytes([0xB7 + len(lb)]) + lb + b
    if isinstance(val, int):
        if val == 0:
            return b"\x80"
        b = val.to_bytes((val.bit_length() + 7) // 8, "big")
        return rlp_encode(b)
    if isinstance(val, list):
        body = b"".join(rlp_encode(x) for x in val)
        if len(body) <= 55:
            return bytes([0xC0 + len(body)]) + body
        lb = len(body).to_bytes((len(body).bit_length() + 7) // 8, "big")
        return bytes([0xF7 + len(lb)]) + lb + body
    raise TypeError(f"unsupported type for RLP encoding: {type(val)}")


def decode_attribute(b: bytes) -> Attr:
    name_b = b[:32].rstrip(b"\x00")
    name = name_b.decode("ascii")
    tid = int.from_bytes(b[32:64], "big")
    indexed = bool(tid & 0x80)
    type_code = tid & 0x7F
    type_map = {1: "bool", 2: "i32", 3: "u64", 4: "u256", 5: "dec", 7: "bytes", 8: "str", 9: "addr", 10: "key"}  # attributes-and-types.md
    typ = type_map.get(type_code, "bytes")
    val_off = int.from_bytes(b[64:96], "big")
    val_len = int.from_bytes(b[val_off : val_off + 32], "big")
    raw_val = b[val_off + 32 : val_off + 32 + val_len]
    val: Any = raw_val
    if typ == "str":
        val = raw_val.decode("utf-8")
    elif typ == "bool":
        val = bool(raw_val[0]) if raw_val else False
    elif typ == "i32":
        val = int.from_bytes(raw_val, "big", signed=True)
    elif typ in ("u64", "u256", "dec"):
        val = int.from_bytes(raw_val, "big")
    return Attr(name, typ, indexed, val)


def decode_attrs_array(b: bytes) -> tuple[Attr, ...]:
    n = int.from_bytes(b[:32], "big")
    offsets = [int.from_bytes(b[32 + i * 32 : 64 + i * 32], "big") for i in range(n)]
    attrs = []
    for off in offsets:
        attrs.append(decode_attribute(b[32 + off :]))
    return tuple(attrs)


def decode_execute(calldata: bytes) -> list[Op]:
    if len(calldata) < 4 or calldata[:4] != EXECUTE_SELECTOR:
        raise ValueError("invalid execute selector")
    b = calldata[4:]
    arr_off = int.from_bytes(b[:32], "big")
    arr = b[arr_off:]
    n_ops = int.from_bytes(arr[:32], "big")
    offsets = [int.from_bytes(arr[32 + i * 32 : 64 + i * 32], "big") for i in range(n_ops)]
    ops: list[Op] = []
    for off in offsets:
        item = arr[32 + off :]
        opcode = int.from_bytes(item[:32], "big")
        data_off = int.from_bytes(item[32:64], "big")
        data_len = int.from_bytes(item[data_off : data_off + 32], "big")
        op_data = item[data_off + 32 : data_off + 32 + data_len]
        if opcode == 1:  # CREATE
            # encodeAbiParameters(uint128, uint64, uint64, uint8, Attribute[]): a parameter list, no leading offset
            body = op_data
            salt = int.from_bytes(body[:32], "big")
            expires_at = int.from_bytes(body[32:64], "big")
            min_lifetime = int.from_bytes(body[64:96], "big")
            flags = int.from_bytes(body[96:128], "big")
            attrs_off = int.from_bytes(body[128:160], "big")
            attrs = decode_attrs_array(body[attrs_off:])
            ops.append(Op("create", salt=salt, expires_at=expires_at, min_lifetime=min_lifetime, flags=flags, attrs=attrs))
        elif opcode == 2:  # PATCH
            body = op_data  # encodeAbiParameters(bytes32, Attribute[])
            key = body[:32]
            attrs_off = int.from_bytes(body[32:64], "big")
            attrs = decode_attrs_array(body[attrs_off:])
            ops.append(Op("patch", key=key, attrs=attrs))
        elif opcode == 3:  # EXTEND
            key = op_data[:32]
            expires_at = int.from_bytes(op_data[32:64], "big")
            min_lifetime = int.from_bytes(op_data[64:96], "big")
            ops.append(Op("extend", key=key, expires_at=expires_at, min_lifetime=min_lifetime))
        elif opcode == 4:  # TRANSFER
            key = op_data[:32]
            new_owner = op_data[32 + 12 : 64]
            ops.append(Op("transfer", key=key, new_owner=new_owner))
        elif opcode == 5:  # DELETE
            key = op_data[:32]
            ops.append(Op("delete", key=key))
        else:
            raise ValueError(f"unknown opcode {opcode}")
    return ops


def execute_raw_tx(chain: Chain, raw_bytes: bytes) -> str:
    tx_hash = keccak256(raw_bytes)
    tx_hash_hex = "0x" + tx_hash.hex()

    if raw_bytes[0] == 0x02:
        # EIP-1559
        items, _ = rlp_decode(raw_bytes[1:])
        if len(items) != 12:
            raise RpcError(-32602, f"invalid EIP-1559 tx fields count: {len(items)}")
        chain_id = int.from_bytes(items[0], "big")
        nonce = int.from_bytes(items[1], "big")
        to = items[5]
        data = items[7]
        v_parity = int.from_bytes(items[9], "big")
        r = int.from_bytes(items[10], "big")
        s = int.from_bytes(items[11], "big")

        sighash_payload = b"\x02" + rlp_encode(items[:9])
        sighash = keccak256(sighash_payload)
        sender = recover_sender(sighash, r, s, v_parity)
    else:
        # Legacy
        items, _ = rlp_decode(raw_bytes)
        if len(items) != 9:
            raise RpcError(-32602, f"invalid legacy tx fields count: {len(items)}")
        nonce = int.from_bytes(items[0], "big")
        to = items[3]
        data = items[5]
        v = int.from_bytes(items[6], "big")
        r = int.from_bytes(items[7], "big")
        s = int.from_bytes(items[8], "big")

        v_parity = v % 2
        sighash = keccak256(rlp_encode(items[:6]))
        sender = recover_sender(sighash, r, s, v_parity)

    if to.lower() == chain.contract.lower() and len(data) >= 4 and data[:4] == EXECUTE_SELECTOR:
        try:
            ops = decode_execute(data)
            chain.apply_ops(sender, ops, tx_hash=tx_hash, calldata=data)
        except Revert as exc:
            raise RpcError(-32000, f"execution reverted: {exc.name}", exc.to_obj()) from exc
        except Exception as exc:
            raise RpcError(-32000, f"execution failed: {exc}") from exc

    return tx_hash_hex
