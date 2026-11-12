# Tasks: docs-release-1.1.0

Branch `claude/busy-babbage-k4puq6` (restarted from `main` @ adc0aed after #8 merged).

## 1. Verify main after the merge
- [x] 1.1 `daemon` CI green on main @ adc0aed (push run); contract green
- [x] 1.2 SDKs offline: ruff, mypy --strict, golden_check 29/29 (Python); eslint, tsc, golden_check 29/29 (Node)
- [x] 1.3 SDKs online against dev-up.sh: Python 4/4, Node dataset checks pass
- [ ] 1.4 Tag v1.1.0 → release workflow (blocked here: the session may push only its branch, tag push got HTTP 403;
      the owner pushes the tag after this change merges, so the release carries CHANGELOG.md and the new notes)

## 2. Guides (`docs/guide/`)
- [x] 2.1 Overview, REST API (curl), Python SDK, Node.js SDK
- [x] 2.2 Every example executed against the daemon (curl walkthrough, Python and Node scripts); fixed two doc
      errors found that way (`where` string literals need `str('…')`; Node `extend(collection, ttl, options)`)

## 3. Website
- [x] 3.1 `/docs`, `/docs/api`, `/docs/python`, `/docs/node` render docs/guide via `?raw` + marked 18.1.2;
      copy buttons on code, anchors, in-app links between guides, edit-on-GitHub and wiki links
- [x] 3.2 Nav entry; Docs buttons on home and install
- [x] 3.3 Version 1.1.0 (install, build); HNSW limitation and section rewritten (what's next); npm token line
- [x] 3.4 Checked in Chromium at 1280 px and 390 px: no console errors, no horizontal overflow

## 4. Wiki, README, packages, release
- [x] 4.1 wiki.yml + wiki_sync.py (Home, REST-API, Python-SDK, Node.js-SDK, MCP-Server when present, sidebar, footer)
- [ ] 4.2 Owner: enable the wiki and create one page once (the session cannot reach the wiki repository)
- [x] 4.3 README: Documentation section, HNSW in features/search/math/limitations, v1.1.0, npm token
- [x] 4.4 SDK READMEs with quick start (shown on PyPI / GitHub Packages)
- [x] 4.5 CHANGELOG.md; release-notes.md links it and the guides; HNSW limitation updated
