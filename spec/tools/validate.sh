#!/usr/bin/env bash
# Validate the contract. Run from repo root. Used by .github/workflows/contract.yml.
set -euo pipefail
npx --yes @redocly/cli@1.25.11 lint --config spec/redocly.yaml spec/openapi.yaml
python3 -I -c "import json,sys; [json.load(open(p)) for p in sys.argv[1:]]" \
  spec/error-codes.json spec/capabilities.json spec/golden/vectorbytes.json spec/fixtures/demo-dataset.json
# Golden files must be reproducible from the generator.
python3 -I spec/tools/gen_golden.py >/dev/null
git diff --exit-code -- spec/golden spec/fixtures
