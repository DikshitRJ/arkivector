# Daemon Code Documentation (generated)

This folder mirrors `daemon/` file by file, for example `daemon/src/codec/vectorbytes.c` → `docs/daemon/src/codec/vectorbytes.c.md`.

- **Writer:** the `opencode-docs` subagent only (`python3 ~/.claude/bin/docgen.py <file> --root .`). Humans and implementing agents do not hand-edit these files.
- **When:** at the end of every daemon phase, for every file added or changed in that phase (see `PLAN_DAEMON.md`, "Docs" step of each phase).
- **Project-level daemon docs**, also generated: `docs/daemon/DESIGN.md`, `docs/daemon/FLOW.md`, `docs/daemon/CONFIG.md`, `docs/daemon/TESTING.md`.
- **Skipped:** vendored code under `daemon/third_party/`. Copied pgvector/PostgreSQL files under `daemon/src/pgvector/` and `daemon/src/pgcompat/` get **short** docs that point to the upstream file and list only the local `ARKIVECTOR:` deltas.
- **Style:** ASD-STE100 Simplified Technical English (enforced by docgen.py).
