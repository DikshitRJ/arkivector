#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"

SCHEMATHESIS="schemathesis"
if [ -f "$REPO_ROOT/.venv/bin/schemathesis" ]; then
    SCHEMATHESIS="$REPO_ROOT/.venv/bin/schemathesis"
fi

BASE_URL="${URL:-http://127.0.0.1:8080}"
OPENAPI_SPEC="$REPO_ROOT/spec/openapi.yaml"

exec "$SCHEMATHESIS" run "$OPENAPI_SPEC" \
    --base-url "$BASE_URL" \
    --checks all \
    --hypothesis-max-examples 50 \
    --exclude-path-regex '(entities:|/entities$|:batch|qdrant|mcp)' \
    --experimental=openapi-3.1 \
    "$@"
