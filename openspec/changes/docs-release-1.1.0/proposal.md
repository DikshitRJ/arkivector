# Proposal: docs-release-1.1.0

## Why
HNSW now works (change `daemon-hnsw-ci-fix`, merged in DikshitRJ/arkivector#8), but the README, the
website and the release notes still said every search is an exact scan. Users had no guide for
the REST API or the SDKs: `docs/api/` is the contract for implementers, and the SDK package pages
(PyPI, npm) were three lines long.

## What changes
- `docs/guide/`: user guides (overview, REST API with curl, Python SDK, Node.js SDK). Every
  example was run against a live daemon (dev-up.sh with a dev signer) before it was written down.
- Website: a `/docs` section that renders those same Markdown files (one source of truth), plus
  version 1.1.0 and the HNSW status on install, build and what's-next pages.
- GitHub wiki: `.github/workflows/wiki.yml` + `docs/tools/wiki_sync.py` publish `docs/guide/` to
  the wiki on every push to main.
- README, SDK READMEs, `CHANGELOG.md`, release notes template for v1.1.0.

## Out of scope
The MCP guide (next change, together with the MCP fixes). Daemon code is unchanged.
