**Arkivector** is pgvector for [Arkiv](https://arkiv.network): `arkivectord` syncs entities from the Arkiv chain, indexes their vectors with pgvector's own code, and serves vector search over REST. The Python and Node SDKs are thin clients of the daemon. MIT licensed.

## What's new

See [CHANGELOG.md](https://github.com/${REPO}/blob/${TAG}/CHANGELOG.md) for the changes in ${TAG}. Documentation: [arkivector.dikshitrj.in/docs](https://arkivector.dikshitrj.in/docs) (also in the [wiki](https://github.com/${REPO}/wiki)): [installation](https://arkivector.dikshitrj.in/docs/install), [REST API](https://arkivector.dikshitrj.in/docs/api), [Python SDK](https://arkivector.dikshitrj.in/docs/python), [Node.js SDK](https://arkivector.dikshitrj.in/docs/node), [MCP server](https://arkivector.dikshitrj.in/docs/mcp).

## Assets

| File | What |
| :--- | :--- |
| `arkivectord-${VERSION}-linux-x86_64.tar.gz` | Daemon binary for Linux x86_64 (libcurl bundled; needs libuv and OpenSSL 3 from the OS) |
| `arkivector-node-${VERSION}.tgz` | Node.js SDK (`npm install ./arkivector-node-${VERSION}.tgz`) |
| Python wheel / sdist (labelled `arkivector-python-…`) | Python SDK (`pip install <file>`); the file names keep the standard wheel format so pip accepts them |
| `SHA256SUMS` | Checksums |

Container image: `ghcr.io/${OWNER}/arkivectord:${VERSION}` (also `:${MAJOR}` and `:latest`, linux/amd64).

## 1. Get Arkiv access (Rabdi testnet)

1. Create an EVM wallet (MetaMask, Rabby, …). Use a dedicated wallet for the daemon.
2. Fund it with test GLM at https://hub.arkiv.network/faucet.
3. Create an access key at https://hub.arkiv.network/access-keys (sign in with the wallet, pick **Rabdi**). Without a key the daemon uses Arkiv's heavily throttled public pool.
4. Export the wallet's private key (hex). The daemon only needs it for writes; without it, it runs read-only.

## 2. Run the daemon

**Binary**
```bash
curl -LO https://github.com/${REPO}/releases/download/${TAG}/arkivectord-${VERSION}-linux-x86_64.tar.gz
tar xzf arkivectord-${VERSION}-linux-x86_64.tar.gz
cd arkivectord-${VERSION}-linux-x86_64

export ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>"
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY   # paste the wallet key: not echoed, not saved in shell history
export ARKIVECTOR_STORAGE_DATA_DIR="$HOME/.local/share/arkivector"

./bin/arkivectord --config arkivector.example.toml
```

**Docker**
```bash
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY   # paste the wallet key: not echoed, not saved in shell history
docker run -d --name arkivectord -p 127.0.0.1:8080:8080 -v arkivector-data:/data \
  -e ARKIVECTOR_ARKIV_ACCESS_KEY="<access key>" \
  -e ARKIV_PRIVATE_KEY \
  ghcr.io/${OWNER}/arkivectord:${VERSION}
curl http://127.0.0.1:8080/healthz
```
To use a config file: `-v $PWD/config.toml:/etc/arkivector/config.toml` and append `--config /etc/arkivector/config.toml`. A sample is in the image at `/etc/arkivector/arkivector.example.toml`.

### Daemon environment variables

Every config key can be set as `ARKIVECTOR_<SECTION>_<KEY>` (environment overrides the file):

| Variable | Default | Meaning |
| :--- | :--- | :--- |
| `ARKIV_PRIVATE_KEY` | unset | Wallet private key (hex, `0x` optional) used to sign writes. Unset = read-only. Never put it in the config file. |
| `ARKIVECTOR_ARKIV_ACCESS_KEY` | empty | Arkiv Hub access key. |
| `ARKIVECTOR_ARKIV_RPC_URLS` | Rabdi RPC | Comma-separated JSON-RPC URLs. |
| `ARKIVECTOR_ARKIV_WS_URL` | Rabdi WS | WebSocket URL; empty = HTTP polling. |
| `ARKIVECTOR_ARKIV_CHAIN_ID` | `7794095` | Rabdi chain id. |
| `ARKIVECTOR_ARKIV_CONFIRMATIONS` | `0` | Blocks behind head before data is applied (2 recommended). |
| `ARKIVECTOR_SERVER_LISTEN` | `127.0.0.1:8080` (`0.0.0.0:8080` in Docker) | HTTP listen address. |
| `ARKIVECTOR_SERVER_API_KEYS_FILE` | empty | Enables API-key auth (see below). Set this before listening on a public interface. |
| `ARKIVECTOR_STORAGE_DATA_DIR` | `/var/lib/arkivector` (`/data` in Docker) | Local index and state. |
| `ARKIVECTOR_LOG_LEVEL` | `info` | `debug`, `info`, `warn`, `error`. |
| `ARKIVECTOR_LOG_FORMAT` | `text` | `text` or `json`. |

API-key auth file (`api_keys.toml`):
```toml
[[keys]]
id = "app"
key = "<a long random secret>"   # or sha256 = "<hex sha256 of the secret>"
scopes = ["read", "write"]       # read | write | admin
```
Clients then send `Authorization: Bearer <secret>`.

## 3. Use the SDKs

SDKs read `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`) and `ARKIVECTOR_API_KEY` (optional bearer key).

**Python**
```bash
pip install arkivector==${VERSION}
```
```python
from arkivector import Client, EntityInput, F
c = Client()  # ARKIVECTOR_URL / ARKIVECTOR_API_KEY
c.create_collection("my.docs", ("halfvec", 4), {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
                    writers=["<your wallet address>"], attributes={"topic": "str"}, default_ttl={"days": 7})
c.upsert("my.docs", [EntityInput(id="cat", vector=[1, 0, 0, 0], document="cats purr", attributes={"topic": "pets"})],
         wait="receipt")
print(c.search("my.docs", vector=[1, 0.1, 0, 0], k=3, filter=F.eq("topic", "pets")).hits)
```

**Node.js** (GitHub Packages needs a GitHub token with `read:packages`, even for public packages)
```bash
echo "@${OWNER}:registry=https://npm.pkg.github.com" >> .npmrc
read -rs NODE_AUTH_TOKEN && export NODE_AUTH_TOKEN   # GitHub token with read:packages
echo '//npm.pkg.github.com/:_authToken=${NODE_AUTH_TOKEN}' >> .npmrc   # a reference, not the token
npm install @${OWNER}/arkivector@${VERSION}
# or, without a token: npm install https://github.com/${REPO}/releases/download/${TAG}/arkivector-node-${VERSION}.tgz
```
```js
import { Client } from "@${OWNER}/arkivector";
const c = new Client({ url: process.env.ARKIVECTOR_URL ?? "http://127.0.0.1:8080" });
const r = await c.search("my.docs", { vector: [1, 0.1, 0, 0], k: 3 });
console.log(r.hits);
```

Add the daemon wallet's address to each collection's `writers`, otherwise the daemon does not index its own writes.

## Known limitations
- Verified on the Arkiv **Rabdi testnet** only; mainnet is untested.
- HNSW graphs are kept in memory and rebuilt from the local store at startup; rebuilds after many updates run on the sync thread.
- The signer comes from `ARKIV_PRIVATE_KEY` only (keystore and remote signers are not wired); gas uses a fixed limit.
