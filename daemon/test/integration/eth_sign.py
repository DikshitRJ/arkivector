"""Minimal EIP-1559 signing for the integration tests (no third-party Ethereum libraries).

Signs transactions the way a client wallet would (eth_account, viem, MetaMask): type 2, low-s,
RFC 6979 deterministic nonces. Also builds Web3 Secret Storage v3 keystores (pbkdf2) for the
keystore-signer tests. Test keys only.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import sys
import uuid
from pathlib import Path
from typing import Any

from Crypto.Cipher import AES
from Crypto.Hash import keccak as _keccak

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "testkit" / "mock-arkiv"))
from raw_tx import GX, GY, N, pt_mul, rlp_encode  # noqa: E402


def keccak256(b: bytes) -> bytes:
    return _keccak.new(digest_bits=256, data=b).digest()


def address_of(priv: bytes) -> str:
    pt = pt_mul(int.from_bytes(priv, "big"), (GX, GY))
    assert pt is not None
    pub = pt[0].to_bytes(32, "big") + pt[1].to_bytes(32, "big")
    return "0x" + keccak256(pub)[-20:].hex()


def _rfc6979_k(priv: int, h: bytes) -> int:
    x = priv.to_bytes(32, "big")
    v = b"\x01" * 32
    k = b"\x00" * 32
    k = hmac.new(k, v + b"\x00" + x + h, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + x + h, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        cand = int.from_bytes(v, "big")
        if 1 <= cand < N:
            return cand
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def sign_hash(priv: bytes, h: bytes) -> tuple[int, int, int]:
    d = int.from_bytes(priv, "big")
    z = int.from_bytes(h, "big")
    k = _rfc6979_k(d, h)
    pt = pt_mul(k, (GX, GY))
    assert pt is not None
    r = pt[0] % N
    s = pow(k, -1, N) * (z + r * d) % N
    parity = pt[1] & 1
    if s > N // 2:  # EIP-2 low-s
        s = N - s
        parity ^= 1
    return r, s, parity


def _q(v: Any) -> int:
    if isinstance(v, int):
        return v
    s = str(v)
    return int(s, 16) if s.startswith("0x") else int(s)


def sign_tx(priv: bytes, tx: dict[str, Any]) -> str:
    """Sign a transaction dict (snake_case as returned by entities:prepare, or camelCase as a wallet
    receives it) and return the raw signed transaction as 0x-hex."""

    def g(*names: str) -> Any:
        for n in names:
            if n in tx:
                return tx[n]
        raise KeyError(names[0])

    fields = [
        _q(g("chain_id", "chainId")),
        _q(g("nonce")),
        _q(g("max_priority_fee_per_gas", "maxPriorityFeePerGas")),
        _q(g("max_fee_per_gas", "maxFeePerGas")),
        _q(g("gas", "gas_estimate")),
        bytes.fromhex(str(g("to"))[2:]),
        _q(tx.get("value", 0)),
        bytes.fromhex(str(g("data", "input"))[2:]),
        [],
    ]
    sighash = keccak256(b"\x02" + rlp_encode(fields))
    r, s, parity = sign_hash(priv, sighash)
    return "0x" + (b"\x02" + rlp_encode(fields + [parity, r, s])).hex()


def make_keystore(priv: bytes, password: str, iterations: int = 1024) -> dict[str, Any]:
    """Web3 Secret Storage v3 with pbkdf2-hmac-sha256 and aes-128-ctr."""
    salt = os.urandom(32)
    iv = os.urandom(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    ct = AES.new(dk[:16], AES.MODE_CTR, nonce=b"", initial_value=iv).encrypt(priv)
    mac = keccak256(dk[16:32] + ct)
    return {
        "version": 3,
        "id": str(uuid.uuid4()),
        "address": address_of(priv)[2:],
        "crypto": {
            "cipher": "aes-128-ctr",
            "cipherparams": {"iv": iv.hex()},
            "ciphertext": ct.hex(),
            "kdf": "pbkdf2",
            "kdfparams": {"c": iterations, "dklen": 32, "prf": "hmac-sha256", "salt": salt.hex()},
            "mac": mac.hex(),
        },
    }


def write_keystore(path: Path, priv: bytes, password: str) -> None:
    path.write_text(json.dumps(make_keystore(priv, password)))
