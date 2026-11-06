#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
PID_DIR="${TMPDIR:-/tmp}/arkivector-dev-up"

# Pick python environment
PYTHON="python3"
if [ -f "$REPO_ROOT/.venv/bin/python3" ]; then
    PYTHON="$REPO_ROOT/.venv/bin/python3"
fi

down() {
    if [ -d "$PID_DIR" ]; then
        if [ -f "$PID_DIR/daemon.pid" ]; then
            pid=$(cat "$PID_DIR/daemon.pid" || true)
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null || true
                for _ in {1..20}; do
                    if ! kill -0 "$pid" 2>/dev/null; then break; fi
                    sleep 0.1
                done
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi

        if [ -f "$PID_DIR/mock.pid" ]; then
            pid=$(cat "$PID_DIR/mock.pid" || true)
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null || true
                for _ in {1..20}; do
                    if ! kill -0 "$pid" 2>/dev/null; then break; fi
                    sleep 0.1
                done
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi

        if [ -f "$PID_DIR/data_dir" ]; then
            data_dir=$(cat "$PID_DIR/data_dir" || true)
            if [ -n "$data_dir" ] && [ -d "$data_dir" ]; then
                rm -rf "$data_dir"
            fi
        fi

        rm -rf "$PID_DIR"
    fi
}

up() {
    down
    mkdir -p "$PID_DIR"

    DATA_DIR=$(mktemp -d "${TMPDIR:-/tmp}/arkivector-data-XXXXXX")
    echo "$DATA_DIR" > "$PID_DIR/data_dir"

    # 1. Start mock-arkiv on port 8545
    setsid "$PYTHON" "$REPO_ROOT/daemon/testkit/mock-arkiv/server.py" --port 8545 --auto-mine-ms 2000 </dev/null > "$PID_DIR/mock.log" 2>&1 &
    MOCK_PID=$!
    echo "$MOCK_PID" > "$PID_DIR/mock.pid"

    # Wait for mock-arkiv ready
    for _ in {1..50}; do
        if curl -s "http://127.0.0.1:8545/_mock/state" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done

    # 2. Seed demo dataset into mock-arkiv
    "$PYTHON" "$REPO_ROOT/daemon/tools/seed-demo.py" --url "http://127.0.0.1:8545"

    # 3. Create arkivectord config pointing to mock-arkiv
    CONFIG_FILE="$PID_DIR/config.toml"
    cat <<EOF > "$CONFIG_FILE"
[server]
listen = "127.0.0.1:8080"

[arkiv]
rpc_urls = ["http://127.0.0.1:8545"]
ws_url = "ws://127.0.0.1:8545/"
chain_id = 7794095
confirmations = 0

[storage]
data_dir = "$DATA_DIR"
map_size = 104857600

[sync]
poll_interval_ms = 50

[log]
level = "debug"
format = "text"
EOF

    # 4. Start arkivectord
    DAEMON_BIN="$REPO_ROOT/daemon/build/dev/arkivectord"
    if [ ! -x "$DAEMON_BIN" ]; then
        echo "Error: $DAEMON_BIN not found or not executable. Please build dev target first." >&2
        exit 1
    fi

    setsid "$DAEMON_BIN" --config "$CONFIG_FILE" --debug-routes </dev/null > "$PID_DIR/daemon.log" 2>&1 &
    DAEMON_PID=$!
    echo "$DAEMON_PID" > "$PID_DIR/daemon.pid"

    # Wait for HTTP server to respond
    for _ in {1..50}; do
        if curl -s "http://127.0.0.1:8080/readyz" >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done

    # 5. Post demo collection
    COLL_JSON=$("$PYTHON" -c 'import json, sys; print(json.dumps(json.load(open(sys.argv[1]))["collection"]))' "$REPO_ROOT/spec/fixtures/demo-dataset.json")
    # With a signer, the daemon's own address must be an allowed writer or its managed writes are not synced.
    if [ -n "${ARKIV_PRIVATE_KEY:-}" ]; then
        COLL_JSON=$(COLL_JSON="$COLL_JSON" "$PYTHON" -c '
import json, os
from eth_account import Account
c = json.loads(os.environ["COLL_JSON"])
addr = Account.from_key(os.environ["ARKIV_PRIVATE_KEY"]).address.lower()
c["writers"] = sorted(set(w.lower() for w in c.get("writers", [])) | {addr})
print(json.dumps(c))')
    fi
    curl -s -X POST "http://127.0.0.1:8080/v1/collections" \
        -H "Content-Type: application/json" \
        -d "$COLL_JSON" >/dev/null 2>&1 || true

    # 6. Poll /readyz until ready and collection is live
    for _ in {1..100}; do
        RESP=$(curl -s "http://127.0.0.1:8080/readyz" 2>/dev/null || true)
        if echo "$RESP" | grep -q '"demo.docs":"live"'; then
            break
        fi
        sleep 0.1
    done

    echo "ARKIVECTOR_URL=http://127.0.0.1:8080"
}

cmd="${1:-up}"
case "$cmd" in
    up)
        up
        ;;
    down)
        down
        ;;
    *)
        echo "Usage: $0 {up|down}" >&2
        exit 1
        ;;
esac
