# Installation

Arkivector has two parts: the daemon `arkivectord`, which does the work, and optional SDKs. You need the daemon; the SDKs are conveniences over its [REST API](../api/README.md).

## 1. Run the daemon

### Docker (any OS, recommended)

```bash
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY   # paste the wallet key: not echoed, not saved in shell history
docker run -d --name arkivectord \
  -p 127.0.0.1:8080:8080 \
  -v arkivector-data:/data \
  -e ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>" \
  -e ARKIV_PRIVATE_KEY \
  ghcr.io/dikshitrj/arkivectord:latest

curl http://127.0.0.1:8080/healthz      # {"status":"ok"}
```

The image is about 18 MB, runs as a non-root user, keeps its index in the `/data` volume and listens on `0.0.0.0:8080` inside the container. Tags: `:<version>` (e.g. `:1.1.0`), `:<major>`, `:latest`.

### Linux x86_64 binary

```bash
VERSION=1.1.0
curl -LO https://github.com/DikshitRJ/arkivector/releases/download/v${VERSION}/arkivectord-${VERSION}-linux-x86_64.tar.gz
tar xzf arkivectord-${VERSION}-linux-x86_64.tar.gz
cd arkivectord-${VERSION}-linux-x86_64

export ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>"
read -rs ARKIV_PRIVATE_KEY && export ARKIV_PRIVATE_KEY   # paste the wallet key: not echoed, not saved in shell history
export ARKIVECTOR_STORAGE_DATA_DIR="$HOME/.local/share/arkivector"

./bin/arkivectord --config arkivector.example.toml
```

libcurl is bundled. Install libuv and OpenSSL 3 from your distribution (`apt install libuv1 libssl3`, `dnf install libuv openssl`). Check the download against `SHA256SUMS` on the release page.

### Build from source

```bash
git clone https://github.com/DikshitRJ/arkivector && cd arkivector/daemon
sudo apt install ninja-build cmake libuv1-dev libssl-dev pkg-config
cmake --preset release && cmake --build --preset release
./build/release/arkivectord --version
```

The daemon needs libcurl 8.11 or newer (WebSocket support). If your distribution ships an older one, `daemon/tools/ci-install-curl.sh` builds a current one. See [Contributing](../development/README.md) for the other presets.

## 2. Get Arkiv access

Reading works without a wallet. Writing sends Arkiv transactions, which need a signer and a little gas.

1. Create a **fresh** EVM wallet for the daemon (MetaMask, Rabby, or anything that can export a private key). Don't reuse a wallet that holds real funds.
2. Get free test GLM from the faucet: <https://hub.arkiv.network/faucet>.
3. Create an access key at <https://hub.arkiv.network/access-keys> (sign in with the wallet, pick **Rabdi**). Without one the daemon shares Arkiv's heavily throttled public pool.
4. Pass both as environment variables: `ARKIV_PRIVATE_KEY` (hex, `0x` optional) and `ARKIVECTOR_ARKIV_ACCESS_KEY`.

> Put the daemon wallet's address in each collection's `writers` list. A collection only indexes entities created by its writers; without it your writes land on chain but never show up in search.

## 3. Install an SDK (optional)

```bash
pip install arkivector                       # Python 3.9+
```

```bash
echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
read -rs NODE_AUTH_TOKEN && export NODE_AUTH_TOKEN   # GitHub token with read:packages
echo '//npm.pkg.github.com/:_authToken=${NODE_AUTH_TOKEN}' >> .npmrc   # a reference, not the token
npm install @dikshitrj/arkivector            # Node 18+
```

GitHub Packages needs a token even for public packages. Without one, install the tarball attached to the release: `npm install https://github.com/DikshitRJ/arkivector/releases/download/v1.1.0/arkivector-node-1.1.0.tgz`.

Both SDKs find the daemon through `ARKIVECTOR_URL` (default `http://127.0.0.1:8080`) and an optional `ARKIVECTOR_API_KEY`.

Next: [Quick start](quickstart.md).
