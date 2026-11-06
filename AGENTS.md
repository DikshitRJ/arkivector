# AGENTS.md: Operating Manual for AI Agents in the Arkivector Monorepo

> Read this file **completely** before you touch anything. Then read [`CODEBASE.md`](CODEBASE.md) and the plan for your track:
> [`PLAN_DAEMON.md`](PLAN_DAEMON.md) (daemon track) or [`PLAN_SDK.md`](PLAN_SDK.md) (SDK track).
> The architecture is in [`ARCHITECTURE.md`](ARCHITECTURE.md). The daemon↔SDK contract is in [`docs/api/`](docs/api/README.md) + [`spec/`](spec/).

---

## 1. What this repository is

**Arkivector** is "pgvector for Arkiv". It has two independently developed products in one monorepo:

| Product | Path | Language | What it is |
| :--- | :--- | :--- | :--- |
| **Daemon** `arkivectord` | `daemon/` | C11 | Off-chain vector co-processor. Syncs entities from the Arkiv chain, indexes vectors with pgvector's own code (copied from `pgvector/`), and serves REST. |
| **SDKs** | `sdk/{python,node}` (active) · `sdk/{c,cpp,rust}` (**deferred**) | Python, TypeScript (later: C, C++17, Rust) | Thin HTTP clients of the daemon, plus the VectorBytes codec and the pgvector value types. |

**Current scope:** daemon + the Python and Node SDKs. `sdk/c`, `sdk/cpp` and `sdk/rust` are deferred placeholders. Do not work on them until the owner re-enables them.
**Owner decisions on open questions** (frozen on-chain format and API, HNSW spike in D0, Rabdi work at the end, licence deferred, owner `@DikshitRJ`) are in [`PLAN_DAEMON.md` → Decisions](PLAN_DAEMON.md#decisions-on-open-questions-2026-11-06-owner-approved-take-the-best-most-optimistic-path).

The two tracks are built **in parallel by independent agents**. They integrate **only** through the frozen contract in `spec/` and `docs/api/`.

---

## 2. Tracks, ownership and write permissions

| Path | Owner | Who may write |
| :--- | :--- | :--- |
| `daemon/**` | Daemon track | Daemon lead + daemon subagents |
| `docs/daemon/**` | Daemon track | **Only** the `opencode-docs` subagent (generated docs) |
| `.github/workflows/daemon.yml` | Daemon track | Daemon lead / `ci-cd-engineer` |
| `sdk/python/**`, `sdk/node/**`, `sdk/README.md` | SDK track | SDK lead + per-language SDK subagents (each only in its own `sdk/<lang>/`) |
| `sdk/{c,cpp,rust}/**` | SDK track (**deferred**) | Nobody, until the owner re-enables them |
| `.github/workflows/sdk.yml` | SDK track | SDK lead |
| `spec/**`, `docs/api/**` | **Contract (shared, frozen)** | Nobody directly. Changes only via a CCR PR approved by the human owner ([`spec/changes/README.md`](spec/changes/README.md)) |
| `.github/workflows/contract.yml`, `.github/PULL_REQUEST_TEMPLATE.md`, `.github/CODEOWNERS`, `.editorconfig`, `.clang-format` | Shared infra | Daemon lead in phase D0 creates them. Later changes need a note in both tracks' PRs |
| `AGENTS.md`, `CODEBASE.md`, `ARCHITECTURE.md`, `PLAN_*.md` | Orchestrator (human) | Agents propose edits in PR descriptions. A track lead may update **its own** plan's status checkboxes only |
| `pgvector/` | **Read-only reference** (pgvector v0.8.7 upstream) | Nobody. Copy *from* it, never edit it |
| `RESEARCH/`, `QUESTION.md` | Read-only reference | Nobody |
| Root `LICENSE` | Owner | **Deferred.** Do not add a licence or SPDX headers until the owner decides. Upstream notices (`LICENSE.pgvector`, `LICENSE.postgresql`) are still required next to copied code |
| `openspec/` | Both (after `openspec init` in D0) | Each track adds change proposals under `openspec/changes/<track>-<id>/` |

**Hard rule:** never edit files outside your track's paths. If you need something from the other track, write a CCR (contract) or open an issue titled `[daemon→sdk]` / `[sdk→daemon]`.

---

## 3. The contract (how two parallel tracks integrate)

1. **Source of truth:** `spec/openapi.yaml`, `spec/error-codes.json`, `spec/capabilities.json`, `spec/golden/vectorbytes.json`, `spec/fixtures/demo-dataset.json`, and `docs/api/*.md`. When code and contract disagree, the code is wrong.
2. **Golden files are generated.** Run `python3 -I spec/tools/gen_golden.py`. Never edit `spec/golden/*` or `spec/fixtures/*` by hand.
3. **Validate the contract** with `spec/tools/validate.sh` (Redocly lint + JSON parse + golden reproducibility).
4. **SDKs develop against the Prism mock** (`npx --yes @stoplight/prism-cli@5.12.0 mock -p 4010 spec/openapi.yaml`) until milestone M2, then against `daemon/tools/dev-up.sh`.
5. **The daemon proves conformance** with schemathesis contract tests (`daemon/test/contract/run.sh`) and with `daemon/test/integration/test_golden_dataset.py`.
6. **Features roll out through capabilities.** The daemon lists implemented features in `GET /v1/version.capabilities`, and returns `501 not_implemented` for the rest. SDKs implement everything from the start and check capabilities at runtime.
7. **Milestones** M0–M4 synchronise the tracks ([`docs/api/sdk-guidelines.md §7`](docs/api/sdk-guidelines.md#7-integration-milestones-shared-with-plan_daemonmd--plan_sdkmd)). A track announces a milestone by creating the git tag `milestone/M<n>-daemon` or `milestone/M<n>-sdk-<lang>` on `main` after the merge.

---

## 4. Git workflow

- **Never commit directly to `main`.** Branch per phase and module:
  - daemon: `daemon/d<phase>-<topic>` (e.g. `daemon/d1-kernels`)
  - sdk: `sdk/<lang>-s<phase>-<topic>` (e.g. `sdk/python-s3-search`)
  - contract: `contract/ccr-<nnnn>-<topic>`
- **Conventional Commits**, with the scope taken from this list:
  `build(daemon)`, `feat(pgcompat)`, `feat(pgvector)`, `feat(codec)`, `feat(kernels)`, `feat(arkiv)`, `feat(store)`, `feat(index)`, `feat(filter)`, `feat(sync)`, `feat(query)`, `feat(write)`, `feat(server)`, `feat(testkit)`, `test(daemon)`, `docs(daemon)`, `perf(daemon)`, `fix(<scope>)`, `ci`, `feat(sdk-c)`, `feat(sdk-cpp)`, `feat(sdk-rust)`, `feat(sdk-python)`, `feat(sdk-node)`, `build(sdk-<lang>)`, `contract`.
- One logical change per commit. The commit body says **what** and **why**, and for copied code **from where** (`pgvector v0.8.7 src/halfutils.c, verbatim`).
- Add the attribution trailer your harness requires (e.g. `Co-Authored-By:` line) at the end of the message.
- Each phase ends in **one PR** to `main`, using the PR template. The PR merges only when CI is green (§7) and the review subagent's verified findings are resolved.
- Commit or push only as the plan instructs. Never force-push `main`. Never rewrite other agents' commits.

---

## 5. Subagent roster and when to use each

The track lead (the agent executing a plan) is the **orchestrator** of its track. It writes the core code itself and delegates the work below. The phase sections of each plan say exactly which subagents to spawn, with prompts.

| Subagent (`subagent_type`) | Backend | Use for | Track |
| :--- | :--- | :--- | :--- |
| `opencode-docs` | OpenCode + local Ollama model | Per-file docs mirror under `docs/daemon/`, plus `docs/daemon/{DESIGN,FLOW,CONFIG,TESTING}.md` | **Daemon only** |
| `gemini-integration-tester` | Antigravity CLI (`agy`, Gemini) | Write and run integration tests (`daemon/test/integration/`, `daemon/test/parity/`, `daemon/test/contract/`). Continuous verification after new code (run in background) | **Daemon only** |
| `gemini-reviewer` | Antigravity CLI (`agy`, Gemini) | Independent cold review of every phase PR diff | **Daemon only** |
| `security-reviewer` | Claude | Extra review of untrusted-input parsers, signer, auth (phases D1, D2, D5, D6) | Daemon |
| `ci-cd-engineer` | Claude | GitHub Actions workflows (D0, D6 release) | Daemon (+ shared infra) |
| `dependency-auditor` | Claude | Licence and CVE audit of vendored libraries (D0, D6) | Daemon |
| `performance-profiler` | Claude | Kernel and search benchmarks, regressions (D1, D4) | Daemon |
| `backend-implementer` | Claude | Bounded, parallelisable implementation lanes (with `isolation: "worktree"`) | Both |
| `explorer` | Claude | Read-only reconnaissance of `pgvector/` or `RESEARCH/` when a plan step says "read" and the lead wants a summary | Both |

**Fallback when a backend is unavailable:** if `agy` rejects the configured Gemini model, run `agy models` and use the closest Gemini flash/pro model. If `agy` itself is unavailable, use `test-writer` instead of `gemini-integration-tester` and `code-reviewer` instead of `gemini-reviewer`. If Ollama or OpenCode is down, `opencode-docs` uses its hosted-model fallback (see its definition). Record any fallback in the PR.

**SDK track rule:** the SDK track has **no documentation, test or review subagents** (explicit project decision). SDK agents verify themselves with `examples/golden_check.*` and the Prism mock.

### 5.1 Handoff protocol (keep subagent context small)

1. Before spawning, the lead stores the task context:
   - If MemPalace is available: `mempalace_task_create` with the phase goal, file list, constraints, and acceptance criteria. Pass **only the task id** plus a 5-line summary to the subagent.
   - If not: pass a **condensed brief** (≤ 40 lines) listing goal, exact files, commands, constraints, and acceptance criteria. Never paste whole files or whole plans.
2. The subagent reads what it needs itself (files, MemPalace, codebase graph).
3. Verification subagents (`gemini-integration-tester`) run with `run_in_background: true` immediately after a code commit. The lead continues and handles the report when it arrives.
4. The lead **re-verifies** every claim a subagent makes (re-runs the tests, reads the code at cited lines) before acting on it.
5. Persist decisions: architecture-level decisions go to MemPalace (`mempalace_kg_add`) and to an OpenSpec change. Phase status goes into the plan's checkboxes.

### 5.2 Standard prompts (fill in the `<…>`)

**Docs (daemon):**
```
subagent_type: opencode-docs
Document the daemon files changed on branch <branch> vs main.
Files: <output of: git diff --name-only main...HEAD -- daemon ':!daemon/third_party'>
Rules: per-file mirror at docs/<path>.md (docgen.py --root .). Files under daemon/src/pgvector/ and daemon/src/pgcompat/
that are copied from upstream get a SHORT doc: upstream path + tag v0.8.7 (or PostgreSQL tag), reuse mode, and the
ARKIVECTOR: deltas only. Then update docs/daemon/DESIGN.md and docs/daemon/FLOW.md sections for module(s) <modules>.
Write only under docs/daemon/. Report missing/skipped files.
```

**Integration tests (daemon, background):**
```
subagent_type: gemini-integration-tester   (run_in_background: true)
Task: integration tests for daemon phase <Dn> — <feature>. Context: MemPalace task <id> | brief below.
Build: cmake --preset dev && cmake --build --preset dev   (binary: daemon/build/dev/arkivectord)
Harness: pytest in daemon/test/integration (conftest.py starts daemon/testkit/mock-arkiv and the daemon with a temp data dir).
Scenarios: <list from the phase section, including failure paths>.
Rules: edit only daemon/test/**; never edit production code; report real bugs with file:line and a repro.
Run: pytest -q daemon/test/integration -k <marker>   and report exact pass/fail output.
```

**Review (daemon):**
```
subagent_type: gemini-reviewer
Review branch <branch> vs main (git diff main...HEAD -- daemon). Focus: <phase-specific focus>.
Also check: copied pgvector code unchanged except ARKIVECTOR-marked deltas; contract conformance with spec/openapi.yaml
and docs/api/*.md; memory safety (bounds, lifetimes, arena use, longjmp unwinding with held locks).
Report file:line, severity, failure scenario. Do not edit files.
```

---

## 6. Coding standards

### 6.1 Daemon (C11)

- **Directory rules**
  - `daemon/src/pgvector/`: copies of pgvector v0.8.7. Keep upstream formatting (pgindent style) and names. The only allowed edits are `#ifndef ARKIVECTOR … #endif` guards around disk/SQL code and lines marked `/* ARKIVECTOR: <reason> */`. Every file is listed in `PROVENANCE.md` with mode V/F/S/P (see ARCHITECTURE §5.1).
  - `daemon/src/pgcompat/`: PostgreSQL API shims and verbatim PostgreSQL files (`list.c`, `pairingheap.c`, `simplehash.h`, `relptr.h`, `f2s.c`). Stub headers exist only to satisfy `#include`s. Each stub is recorded in `STUBS.md`.
  - Everything else is Arkivector code: `av_` prefix for public symbols, `snake_case`, one module per directory, and each `.c` has a matching `.h` with the public API.
- **Memory:** use `av_arena_*` (core/arena.h) for request- and batch-scoped memory. pgvector code uses `palloc`, which maps onto the current arena. `malloc`/`free` are allowed only for long-lived objects with a clear owner. Every function documents ownership of returned pointers.
- **Errors:** Arkivector code returns `av_status` enums and fills `av_err *` (code from `spec/error-codes.json`, message). `ereport(ERROR)` from pgvector code is caught at the `PG_TRY` boundary in `pgcompat/src/elog.c` and converted to `av_err`.
- **Untrusted input** (chain data, HTTP, RPC JSON) is parsed only by code with a fuzz target in `daemon/test/fuzz/`. Check every length before allocating.
- **Threads:** the libuv loop thread never blocks and never runs CPU-heavy work. Data structures shared across threads document their lock or atomic protocol at the top of the header.
- **Warnings:** our code builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`. `src/pgvector/` and `third_party/` build with upstream flags only.
- **Formatting:** `clang-format` with the root `.clang-format` (our code only; `src/pgvector` and `src/pgcompat` verbatim files are excluded).
- **Logging:** `av_log(level, module, fmt, …)`. Never log secrets (access keys, private keys, API keys).

### 6.2 SDKs

| Lang | Style / lint | Build | Notes |
| :--- | :--- | :--- | :--- |
| C *(deferred)* | `.clang-format`, `-Wall -Wextra -Werror` | CMake ≥ 3.20 | C11, libcurl, vendored yyjson. `av_` prefix shared with the daemon is fine because they are separate binaries, but the SDK must not include daemon headers |
| C++ *(deferred)* | `.clang-format`, `-Wall -Wextra -Werror` | CMake | C++17, wraps the C SDK |
| Rust *(deferred)* | `cargo fmt`, `cargo clippy -- -D warnings` | Cargo | MSRV 1.75 |
| Python | `ruff check`, `ruff format`, `mypy --strict` | `pyproject.toml` (hatchling) | ≥ 3.9, stdlib-only core |
| Node | `eslint`, `tsc --noEmit`, `prettier --check` | `tsc` → `dist/` | ESM, Node ≥ 18, zero runtime deps |

SDKs **must not** import or copy daemon code. They may copy **pgvector** code where useful (licence: PostgreSQL License; keep the notice), e.g. the half-float software conversion from `pgvector/src/halfutils.h` in the C SDK.

---

## 7. Quality gates (CI)

- `.github/workflows/daemon.yml` runs on every push and PR on every branch (`'**'`).
  - **Phase 1** (code quality & compilation): configure + build (gcc, clang), clang-format check, clang-tidy, cppcheck.
  - **Phase 2** (integration tests): unit tests (ASan + UBSan), integration tests vs mock-arkiv, contract tests (schemathesis), pgvector parity tests.
  - The green tick requires both phases.
- `.github/workflows/sdk.yml` (all branches): build + lint + `golden_check` offline for **Python and Node** (other languages are added when re-enabled). No test suites.
- `.github/workflows/contract.yml` (all branches): `spec/tools/validate.sh`.

---

## 8. Commands (valid once the corresponding phase has landed)

```bash
# Contract
spec/tools/validate.sh
python3 -I spec/tools/gen_golden.py
npx --yes @stoplight/prism-cli@5.12.0 mock -p 4010 spec/openapi.yaml

# Daemon (from daemon/)
cmake --preset dev && cmake --build --preset dev
ctest --preset dev                                   # unit tests
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
pytest -q test/integration                           # needs: pip install -r test/integration/requirements.txt
test/contract/run.sh                                 # schemathesis vs a running daemon
docker compose -f test/parity/docker-compose.yml up -d && pytest -q test/parity
tools/dev-up.sh                                      # mock-arkiv + daemon + demo.docs on :8080

# SDKs
(cd sdk/python && ruff check . && mypy --strict src && python -m examples.golden_check)
(cd sdk/node && npm ci && npm run lint && npm run build && node dist/examples/golden_check.js)
```

---

## 9. Definition of Done (every phase)

1. All plan steps for the phase are checked off in the plan file.
2. CI is green on the phase branch.
3. Daemon only: `gemini-integration-tester` report is green, `gemini-reviewer` findings are resolved or justified in the PR, and `opencode-docs` has documented every changed file.
4. Contract conformance: the daemon passes contract tests for every capability it advertises. SDKs pass `golden_check` (offline, plus online once M2 exists).
5. PR merged to `main`, milestone tag created if the phase completes a milestone.
6. Phase summary written to MemPalace (or appended to the plan's "Status log").

---

## 10. Where knowledge lives

| Need | Read |
| :--- | :--- |
| Overall design and rationale | `ARCHITECTURE.md` |
| Arkiv RPC, query language, events, limits | `RESEARCH/arkiv-docs/` (start with `README.md`, then `02-json-rpc-protocol/`, `03-evm-mutations-and-contract-spec/`, `04-query-engine-and-ast/`) |
| pgvector internals | `RESEARCH/pgvector/` (start with `README.md`, then `indexes/hnsw_*.md`, `core/*_type.md`) |
| pgvector source to copy | `pgvector/src/` (v0.8.7). Its regression tests are in `pgvector/test/sql` + `pgvector/test/expected`, and TAP recall tests in `pgvector/test/t` |
| API contract | `docs/api/`, `spec/` |
| Repo map | `CODEBASE.md` |
