# Testing

## Daemon

```bash
cd daemon
ctest --preset dev                                         # unit tests
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan

pip install -r test/integration/requirements.txt -r testkit/mock-arkiv/requirements.txt
AV_REQUIRE_DAEMON=1 pytest -q test/integration            # daemon against mock-arkiv

docker compose -f test/parity/docker-compose.yml up -d --wait
pytest -q test/parity                                     # against real pgvector 0.8.7
```

| Suite | What it proves |
| :--- | :--- |
| Unit (`test/unit`) | Kernels, codecs, filters, planner, segments, HNSW, signer, REST handlers |
| Integration (`test/integration`) | The daemon end to end against mock-arkiv: sync, reorgs, TTL, search, HNSW recall and distances, filters, writes, auth, MCP, Qdrant, the golden dataset |
| Parity (`test/parity`) | Distances, text I/O and error messages match real pgvector |
| Fuzz (`test/fuzz`) | HTTP, JSON-RPC responses, filters, vector literals, VectorBytes, ABI |

## SDKs

```bash
(cd sdk/python && ruff check . && mypy --strict src && python -m examples.golden_check)
(cd sdk/node && npm ci && npm run lint && npm run build && node dist/examples/golden_check.js)
```

`golden_check` verifies the VectorBytes codec against the 29 golden vectors offline, and with `ARKIVECTOR_URL` set, the demo-dataset queries against a live daemon.

## Contract

```bash
spec/tools/validate.sh                      # OpenAPI lint, JSON parse, golden reproducibility
python3 -I spec/tools/gen_golden.py         # regenerate golden files (never edit them by hand)
```

## CI

| Workflow | Runs |
| :--- | :--- |
| `daemon.yml` | Phase 1: gcc and clang builds, clang-format, clang-tidy, cppcheck, provenance check. Phase 2: unit tests under ASan/UBSan and TSan, integration tests, parity suite, fuzz smoke (60 s per target; 5 min nightly) |
| `sdk.yml` | Python and Node lint, build, offline golden check |
| `contract.yml` | `spec/tools/validate.sh` |
| `wiki.yml` | Publishes `docs/wiki/` to the GitHub wiki on push to main |
| `release.yml` | Release artifacts on version tags ([Releasing](releasing.md)) |
