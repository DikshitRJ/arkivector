# Install and get started

Everything you need to go from nothing to a working vector search on Arkiv: the daemon, an Arkiv wallet and access key, and an SDK. It takes about ten minutes. After that, the [Quick start](getting-started/quickstart.md) walks you through your first collection.

Arkivector has two parts: the daemon `arkivectord`, which does the work, and optional SDKs. You always need the daemon; the SDKs are conveniences over its [REST API](api/README.md).

## 1. Get Arkiv access

Reading works without a wallet. Writing sends Arkiv transactions, which need a signer and a little gas. The signer can be the daemon (steps below), or each user's own wallet, in which case the daemon needs no key at all ([Signing writes](getting-started/signing.md)).

1. Create a **fresh** EVM wallet for the daemon (MetaMask, Rabby, or anything that can export a private key). Don't reuse a wallet that holds real funds.
2. Get free test GLM from the faucet: <https://hub.arkiv.network/faucet>.
3. Create an access key at <https://hub.arkiv.network/access-keys> (sign in with the wallet, pick **Rabdi**). Without one the daemon shares Arkiv's heavily throttled public pool.
4. Export the wallet's private key (hex). The daemon only needs it for managed writes; without it, it can still search and relay writes signed by your users' wallets. In production prefer an encrypted keystore or a remote signer ([Signing writes](getting-started/signing.md#the-daemons-signer)).

| Variable | Required | What it does |
| :--- | :--- | :--- |
| `ARKIV_PRIVATE_KEY` | For managed writes | Hex private key that signs transactions (`0x` optional). Without it (and without a keystore or remote signer) the daemon can't sign; writes signed by your own wallet still work. Keep it out of config files and shell history |
| `ARKIVECTOR_ARKIV_ACCESS_KEY` | Recommended | Your Arkiv Hub access key, so you are not on the throttled public pool |
| `ARKIVECTOR_STORAGE_DATA_DIR` | Binary only | Where the local index lives. The Docker image uses `/data` |
| `ARKIVECTOR_URL` | SDKs | Where the SDKs find the daemon. Defaults to `http://127.0.0.1:8080` |

Every other setting is in [Configuration](getting-started/configuration.md).

## 2. Run the daemon

### Docker (any OS, recommended)

```bash
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY   # paste the wallet key: not echoed, not saved in shell history
docker run -d --name arkivectord \
  -p 127.0.0.1:8080:8080 \
  -v arkivector-data:/data \
  -e ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>" \
  -e ARKIV_PRIVATE_KEY \
  ghcr.io/dikshitrj/arkivectord:latest
```

The image is about 18 MB, runs as a non-root user, keeps its index in the `/data` volume and listens on `0.0.0.0:8080` inside the container. Tags: `:<version>` (e.g. `:1.2.0`), `:<major>`, `:latest`. The index survives restarts in the `arkivector-data` volume.

### Linux x86_64 binary

```bash
VERSION=1.2.0
curl -LO https://github.com/DikshitRJ/arkivector/releases/download/v${VERSION}/arkivectord-${VERSION}-linux-x86_64.tar.gz
tar xzf arkivectord-${VERSION}-linux-x86_64.tar.gz
cd arkivectord-${VERSION}-linux-x86_64

export ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>"
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY   # paste the wallet key: not echoed, not saved in shell history
export ARKIVECTOR_STORAGE_DATA_DIR="$HOME/.local/share/arkivector"

./bin/arkivectord --config arkivector.example.toml
```

libcurl is bundled. Install libuv and OpenSSL 3 from your distribution (`apt install libuv1 libssl3`, `dnf install libuv openssl`). Check the download against `SHA256SUMS` on the [release page](https://github.com/DikshitRJ/arkivector/releases). macOS and Windows: use Docker.

### Build from source

```bash
git clone https://github.com/DikshitRJ/arkivector && cd arkivector/daemon
sudo apt install ninja-build cmake libuv1-dev libssl-dev pkg-config
cmake --preset release && cmake --build --preset release
./build/release/arkivectord --version
```

The daemon needs libcurl 8.11 or newer (WebSocket support). If your distribution ships an older one, `daemon/tools/ci-install-curl.sh` builds a current one. See [Contributing](development/README.md) for the other presets.

## 3. Check that it runs

```bash
curl -s http://127.0.0.1:8080/healthz          # {"status":"ok"}
curl -s http://127.0.0.1:8080/v1/version       # versions, "writes_enabled": true when the key was picked up
```

`writes_enabled: false` means the daemon has no signer (it did not see `ARKIV_PRIVATE_KEY`, and no keystore or remote signer is configured): it can still search, and accept writes signed by your own wallet, but it can't sign. `signer.kind` says which signer is in use.

> **Find your wallet address** (you need it in the next step): it is the address of the wallet whose key you exported. Put it in each collection's `writers` list. A collection only indexes entities created by its writers; without it your writes land on chain but never show up in search.

## 4. Install an SDK (optional)

```bash
pip install arkivector                       # Python 3.9+
```

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
read -rs NODE_AUTH_TOKEN && export NODE_AUTH_TOKEN   # GitHub token with read:packages
echo '//npm.pkg.github.com/:_authToken=${NODE_AUTH_TOKEN}' >> .npmrc   # a reference, not the token
npm install @dikshitrj/arkivector            # Node 18+
```

GitHub Packages needs a token even for public packages. Without one, install the tarball attached to the release: `npm install https://github.com/DikshitRJ/arkivector/releases/download/v1.2.0/arkivector-node-1.2.0.tgz`.

Both SDKs find the daemon through `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`) and an optional `ARKIVECTOR_API_KEY`. Plain HTTP works too: see the [REST API](api/README.md).

## 5. Get started

You are set up. Next:

- [Quick start](getting-started/quickstart.md): your first collection, writes and searches, in curl, Python or Node.
- [Connect an AI agent](mcp/setup.md) to the daemon's MCP server.
- [Concepts](getting-started/concepts.md): collections, entities, blocks, lifetimes.
- Before exposing the daemon beyond localhost: [Security](operations/security.md).
