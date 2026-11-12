# Contributing

## Repository layout

```
daemon/          arkivectord: C11 sources, tests, fuzzers, mock Arkiv node, Dockerfile
  src/pgvector/  pgvector 0.8.7 code (copied, see PROVENANCE.md)
  src/pgcompat/  PostgreSQL API shims and verbatim PostgreSQL files
  test/          unit (greatest), integration (pytest), parity (pgvector in Postgres), fuzz
  testkit/       mock-arkiv: a local Arkiv JSON-RPC/WebSocket node for tests
sdk/python/      Python SDK (stdlib only, typed, LangChain and LlamaIndex adapters)
sdk/node/        Node.js / TypeScript SDK (zero runtime dependencies, LangChain.js adapter)
spec/            The REST contract: OpenAPI, error codes, capabilities, golden test vectors
docs/wiki/       These documentation pages (website /docs and GitHub wiki)
docs/api/        The contract in prose, field by field
openspec/        Spec-driven change proposals and task logs
web/             The website (React + Vite, Cloudflare Workers)
```

Process documents: [`AGENTS.md`](https://github.com/DikshitRJ/arkivector/blob/main/AGENTS.md) (workflow, ownership, coding standards), [`CODEBASE.md`](https://github.com/DikshitRJ/arkivector/blob/main/CODEBASE.md) (file map), [`ARCHITECTURE.md`](https://github.com/DikshitRJ/arkivector/blob/main/ARCHITECTURE.md) (design).

## Building the daemon

```bash
sudo apt install ninja-build cmake clang libuv1-dev libssl-dev pkg-config
# libcurl >= 8.11 (WebSocket): build one if your distribution is older
CURL_VERSION=8.23.0 CURL_PREFIX=/opt/curl daemon/tools/ci-install-curl.sh
export PKG_CONFIG_PATH=/opt/curl/lib/pkgconfig LD_LIBRARY_PATH=/opt/curl/lib

cd daemon
cmake --preset dev && cmake --build --preset dev     # build/dev/arkivectord
```

| Preset | Use |
| :--- | :--- |
| `dev` | Debug build, tests |
| `release` | Optimised build |
| `asan` | AddressSanitizer + UBSan |
| `tsan` | ThreadSanitizer (clang) |
| `fuzz` | libFuzzer targets (clang) |

## Running it locally

```bash
daemon/tools/dev-up.sh up        # mock Arkiv on :8545, 200 demo entities, daemon on :8080
ARKIV_PRIVATE_KEY=<any test key> daemon/tools/dev-up.sh up   # with writes against the mock
daemon/tools/dev-up.sh down
```

## Coding standards

- C11 with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`, formatted with clang-format (the CI pins 23.1.4), clean under clang-tidy and cppcheck.
- `av_` prefix, one module per directory, ownership of returned pointers documented.
- Files under `src/pgvector/` and `src/pgcompat/` copied from upstream stay unchanged except marked deltas.
- Every parser of untrusted input has a fuzz target.
- Python SDK: `ruff check`, `ruff format`, `mypy --strict`. Node SDK: `eslint`, `tsc`, `prettier`.

## Workflow

Spec-driven: a change starts as an OpenSpec proposal under `openspec/changes/<name>/` (proposal, design, tasks, requirements), then code, then a pull request using the template. Conventional Commits (`feat(index): …`, `fix(server): …`). CI must be green: see [Testing](testing.md).

The REST contract in `spec/` and `docs/api/` is frozen; changing it needs a contract change request (`spec/changes/README.md`).
