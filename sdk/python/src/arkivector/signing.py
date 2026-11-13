"""Client-side signing helpers (contract 1.1.0, GitHub issue #12). Stdlib only.

The daemon builds unsigned transactions (``Client.prepare``), *you* sign them with whatever
holds the key, and the daemon broadcasts them and waits for indexing (``Client.submit``).
``Client.upsert_signed`` does all three. A signer is any callable; nothing here imports an
Ethereum library.

``sign`` callables receive a :class:`~arkivector.models.PreparedTransaction` and return the raw
signed transaction: ``bytes``, a ``0x…`` hex string, or an object with ``raw_transaction`` /
``rawTransaction`` (what ``eth_account`` returns). ``send`` callables broadcast it themselves
(a wallet that only offers ``eth_sendTransaction``) and return the transaction hash.

Example with eth_account::

    from eth_account import Account
    from arkivector import Client
    from arkivector.signing import eth_account_signer

    acct = Account.from_key(user_key)
    c = Client("http://127.0.0.1:8080")
    res = c.upsert_signed("notes", [{"id": "a", "vector": [0.1, 0.2, 0.3]}],
                          from_address=acct.address, sign=eth_account_signer(acct))

Handing the transactions to a browser (MetaMask) instead: send ``[tx.to_rpc() for tx in
prepared.transactions]`` to the page, let it call ``ethereum.request({method:
"eth_sendTransaction", params: [tx]})`` for each, post the hashes back, and call
``client.submit(collection, tx_hashes=hashes, wait="indexed")``.
"""

from __future__ import annotations

import re
from typing import Any, Callable, Union

from arkivector.models import PreparedTransaction

RawSigned = Union[bytes, bytearray, str, Any]
SignFn = Callable[[PreparedTransaction], RawSigned]
SendFn = Callable[[PreparedTransaction], str]

_HASH_RE = re.compile(r"^0x[0-9a-fA-F]{64}$")
_RAW_RE = re.compile(r"^0x02(?:[0-9a-fA-F]{2})+$")


def normalize_raw(signed: RawSigned) -> str:
    """Return a signed transaction as ``0x``-hex, from bytes, hex text or an eth_account result."""
    for attr in ("raw_transaction", "rawTransaction"):
        if hasattr(signed, attr):
            signed = getattr(signed, attr)
            break
    if isinstance(signed, (bytes, bytearray)):
        out = "0x" + bytes(signed).hex()
    elif isinstance(signed, str):
        out = signed if signed.startswith(("0x", "0X")) else "0x" + signed
        out = "0x" + out[2:]
    elif hasattr(signed, "hex") and callable(signed.hex):
        h = str(signed.hex())
        out = h if h.startswith("0x") else "0x" + h
    else:
        raise TypeError(f"cannot read a signed transaction from {type(signed).__name__}")
    if not _RAW_RE.match(out):
        raise ValueError("signer did not return a signed EIP-1559 (0x02…) transaction")
    return out.lower()


def normalize_hash(tx_hash: Any) -> str:
    """Return a transaction hash as lowercase ``0x``-hex (accepts bytes / HexBytes / str)."""
    if isinstance(tx_hash, (bytes, bytearray)):
        h = "0x" + bytes(tx_hash).hex()
    else:
        is_obj = hasattr(tx_hash, "hex") and not isinstance(tx_hash, str)
        h = str(tx_hash.hex() if is_obj else tx_hash)
        if not h.startswith("0x"):
            h = "0x" + h
    if not _HASH_RE.match(h):
        raise ValueError(f"send callable returned {h!r}, not a 32-byte transaction hash")
    return h.lower()


def eth_account_signer(account: Any) -> SignFn:
    """A ``sign`` callable for an ``eth_account`` LocalAccount (or anything with
    ``sign_transaction(dict)``), e.g. ``Account.from_key(...)`` or a hardware-wallet account."""

    def sign(tx: PreparedTransaction) -> RawSigned:
        return account.sign_transaction(tx.to_eth_account())

    return sign


def web3_sender(w3: Any) -> SendFn:
    """A ``send`` callable for a web3.py ``Web3`` whose node holds/unlocks the key
    (``w3.eth.send_transaction``); the node broadcasts and returns the hash."""

    def send(tx: PreparedTransaction) -> str:
        d = tx.to_eth_account()
        d["from"] = tx.from_address
        return normalize_hash(w3.eth.send_transaction(d))

    return send
