# Deployment

## Docker Compose

```yaml
services:
  arkivectord:
    image: ghcr.io/dikshitrj/arkivectord:1.2.0
    restart: unless-stopped
    ports: ["127.0.0.1:8080:8080"]
    volumes:
      - arkivector-data:/data
      - ./api_keys.toml:/etc/arkivector/api_keys.toml:ro
    environment:
      ARKIVECTOR_ARKIV_ACCESS_KEY: ${ARKIV_ACCESS_KEY}
      ARKIV_PRIVATE_KEY: ${ARKIV_PRIVATE_KEY}          # omit if users sign with their own wallets
      ARKIVECTOR_ARKIV_CONFIRMATIONS: "2"
      ARKIVECTOR_SERVER_API_KEYS_FILE: /etc/arkivector/api_keys.toml
    healthcheck:
      test: ["CMD", "wget", "-qO-", "http://127.0.0.1:8080/healthz"]
      interval: 10s
volumes:
  arkivector-data:
```

The image runs as a non-root user and keeps state in `/data`. Pin a version tag in production (`:1.2.0`, or `:1` for the latest 1.x).

## systemd (binary)

```ini
# /etc/systemd/system/arkivectord.service
[Unit]
Description=Arkivector daemon
After=network-online.target

[Service]
User=arkivector
ExecStart=/opt/arkivectord/bin/arkivectord --config /etc/arkivector/config.toml
EnvironmentFile=/etc/arkivector/secrets.env      # ARKIV_PRIVATE_KEY=..., ARKIVECTOR_ARKIV_ACCESS_KEY=...
Restart=on-failure
StateDirectory=arkivector                        # /var/lib/arkivector
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/var/lib/arkivector

[Install]
WantedBy=multi-user.target
```

Keep `secrets.env` readable only by the service user (`chmod 600`).

To keep the private key out of the environment, use an encrypted keystore with a systemd encrypted credential for its passphrase:

```ini
[Service]
LoadCredentialEncrypted=keystore-pass:/etc/credstore.encrypted/keystore-pass
Environment=ARKIVECTOR_WRITE_SIGNER_KIND=keystore
Environment=ARKIVECTOR_WRITE_SIGNER_KEYSTORE_PATH=/etc/arkivector/signer.json
Environment=ARKIVECTOR_WRITE_SIGNER_PASSPHRASE_CREDENTIAL=keystore-pass
```

or a remote signer (Web3Signer, Clef) so the daemon never holds the key. See [Signing writes](../getting-started/signing.md#the-daemons-signer).

## Behind a reverse proxy

The daemon speaks plain HTTP; terminate TLS in front of it. Caddy:

```
arkivector.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

nginx:

```nginx
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host $host;
    proxy_read_timeout 90s;          # writes with wait=indexed can take a while
}
```

**Turn on API keys before exposing the daemon** ([Security](security.md)).

## Sizing

| Resource | Guide |
| :--- | :--- |
| CPU | Query workers (`server.threads`, default 4) run searches; one more core for sync and the applier |
| Memory | Vectors (2 bytes per dimension for `halfvec`) plus the HNSW graph for hnsw collections; see [Storage](../architecture/storage.md#memory) |
| Disk | LMDB rows (vectors, documents, attributes) plus the flat segment files |
| Network | Bootstrap downloads every entity once; afterwards only changed entities |

## Upgrades

The local index is a cache of the chain. Upgrading is: stop, replace the binary or image, start. If the on-disk format ever changes, start with `--resync` (or a fresh data directory) and the daemon rebuilds from Arkiv. Collections are recreated from the stored definitions automatically.

## Readiness

`/readyz` returns `503` until every collection has finished bootstrapping, so put it behind your load balancer's health check and use `/healthz` for liveness.
