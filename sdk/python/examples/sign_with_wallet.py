"""Write entities signed by the user's own wallet (eth_account); the daemon holds no key.

    pip install eth-account          # only this example needs it; the SDK itself is stdlib-only
    ARKIVECTOR_URL=http://127.0.0.1:8080 USER_PRIVATE_KEY=0x... python examples/sign_with_wallet.py

The wallet's address must be in the collection's ``writers`` (this example creates the collection
with it). Three ways are shown:

1. ``upsert_signed`` with ``eth_account_signer``: prepare -> sign locally -> the daemon broadcasts.
2. The same steps by hand, e.g. to sign on another machine, an HSM or a KMS.
3. ``to_rpc()`` for a browser wallet: send the transaction JSON to the page, let MetaMask sign and
   broadcast it (``eth_sendTransaction``), then ``submit(tx_hashes=...)``. See
   ``sdk/node/examples/wallet_server.ts`` for a runnable page.
"""

import json
import os
import sys
from pathlib import Path

SDK_SRC = Path(__file__).resolve().parent.parent / "src"
if str(SDK_SRC) not in sys.path:
    sys.path.insert(0, str(SDK_SRC))

from arkivector import Client, eth_account_signer  # noqa: E402


def main() -> None:
    try:
        from eth_account import Account  # type: ignore[import-not-found]
    except ImportError:
        print("This example needs eth-account: pip install eth-account")
        return
    key = os.environ.get("USER_PRIVATE_KEY")
    if not key:
        print("Set USER_PRIVATE_KEY to a test wallet's private key (never a funded mainnet key).")
        return
    acct = Account.from_key(key)
    c = Client(os.environ.get("ARKIVECTOR_URL", "http://127.0.0.1:8080"))
    v = c.version()
    print(f"daemon {v.daemon_version}, managed signer: {v.signer.kind if v.signer else 'none'}")
    if not c.has_capability("entities.submit"):
        print("This daemon predates client-side submit (contract 1.1.0).")
        return

    name = "wallet.notes"
    if name not in [col.name for col in c.list_collections()]:
        c.create_collection(
            name,
            column=("vector", 3),
            index={"method": "none", "opclass": "vector_l2_ops"},
            writers=[acct.address],
            default_ttl={"days": 7},
        )

    # 1. One call: prepare -> sign with the wallet -> daemon broadcasts and waits for indexing.
    res = c.upsert_signed(
        name,
        [{"id": "fish", "vector": [0.9, 0.1, 0.0], "document": "Fish swim."}],
        from_address=acct.address,
        sign=eth_account_signer(acct),
    )
    print("upsert_signed:", [(r.id, r.key, r.op) for r in res.results], "block", res.block_number)

    # 2. By hand: the prepared transactions are plain data you can sign anywhere.
    prepared = c.prepare(
        name, [{"id": "bird", "vector": [0.1, 0.9, 0.0]}], from_address=acct.address
    )
    raws = [
        acct.sign_transaction(tx.to_eth_account()).raw_transaction for tx in prepared.transactions
    ]
    sub = c.submit(name, signed_transactions=raws, wait="indexed")
    print("submit:", sub.tx_hashes, "created", sub.created_keys)

    # 3. What a browser wallet would receive (JSON for ethereum.request eth_sendTransaction).
    prepared = c.prepare(name, [{"id": "cat", "vector": [0.0, 0.1, 0.9]}], acct.address)
    print("for MetaMask:", json.dumps([tx.to_rpc() for tx in prepared.transactions])[:160], "...")

    hits = c.search(name, [0.9, 0.1, 0.0], k=3).hits
    print("search:", [(h.id, round(h.distance, 4)) for h in hits])


if __name__ == "__main__":
    main()
