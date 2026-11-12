#!/usr/bin/env python3
"""Publish docs/wiki/ to a checked-out GitHub wiki repository (stdlib only).

docs/wiki/ is the single source for the repository docs, the website's /docs section and the
GitHub wiki. GitHub wiki page names must be unique and the wiki has no folders in its URLs, so
each file gets a readable unique name (api/search.md -> "API-Search"), relative links between
pages are rewritten to those names, and _Sidebar.md is generated from SUMMARY.md. Pages this
script published before but whose source is gone are removed (tracked in .arkivector-pages).

Usage: python3 -I docs/tools/wiki_sync.py <wiki-checkout-dir>
"""

from __future__ import annotations

import posixpath
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIKI = ROOT / "docs" / "wiki"
REPO_URL = "https://github.com/DikshitRJ/arkivector"
SITE_URL = "https://arkivector.dikshitrj.in"
MANIFEST = ".arkivector-pages"

FOLDER = {
    "getting-started": "Getting-Started",
    "api": "API",
    "python": "Python",
    "node": "Node.js",
    "mcp": "MCP",
    "architecture": "Architecture",
    "operations": "Operations",
    "development": "Development",
}
INDEX = {
    "": "Home",
    "api": "API",
    "python": "Python-SDK",
    "node": "Node.js-SDK",
    "mcp": "MCP-Server",
    "architecture": "Architecture",
    "operations": "Operations",
    "development": "Development",
    "getting-started": "Getting-Started",
}
LINK = re.compile(r"(\[[^\]]*\]\()([^)\s]+)(\))")


def page_name(rel: str) -> str:
    """docs/wiki-relative path -> unique wiki page name."""
    folder, _, base = rel.rpartition("/")
    stem = base[:-3]
    if stem == "README":
        return INDEX.get(folder, "-".join(p.capitalize() for p in folder.split("/")))
    words = "-".join(w.upper() if w in ("api", "mcp", "ttl", "sdk") else w.capitalize() for w in stem.split("-"))
    prefix = "-".join(FOLDER.get(p, p.capitalize()) for p in folder.split("/")) if folder else ""
    return f"{prefix}-{words}" if prefix else words


def resolve(rel: str, href: str) -> tuple[str, str] | None:
    """Relative markdown link from page rel -> (target rel, '#anchor' or ''), or None if external."""
    if re.match(r"^[a-z]+:", href) or href.startswith(("/", "#")):
        return None
    target, _, anchor = href.partition("#")
    if not target.endswith(".md"):
        return None
    tgt = posixpath.normpath(posixpath.join(posixpath.dirname(rel), target))
    return tgt, ("#" + anchor if anchor else "")


def rewrite(rel: str, text: str, pages: set[str]) -> str:
    def sub(m: re.Match[str]) -> str:
        r = resolve(rel, m.group(2))
        if r is None:
            return m.group(0)
        tgt, anchor = r
        if tgt in pages:
            return f"{m.group(1)}{page_name(tgt)}{anchor}{m.group(3)}"
        return f"{m.group(1)}{REPO_URL}/blob/main/docs/wiki/{tgt}{anchor}{m.group(3)}"

    out, in_code = [], False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith(("```", "~~~")):
            in_code = not in_code
        out.append(line if in_code else LINK.sub(sub, line))
    return "".join(out)


def sidebar(pages: set[str]) -> str:
    """SUMMARY.md -> _Sidebar.md (same tree, wiki links)."""
    text = (WIKI / "SUMMARY.md").read_text(encoding="utf-8")
    lines = [ln for ln in text.splitlines() if not ln.startswith("# ")]
    body = rewrite("SUMMARY.md", "\n".join(lines).strip() + "\n", pages)
    body = re.sub(r"^- ([^\[\n]+)$", r"- **\1**", body, flags=re.M)  # section headings
    return (
        f"### [Arkivector]({SITE_URL})\n\n{body}\n"
        f"[Website]({SITE_URL}/docs) · [Repository]({REPO_URL}) · [Releases]({REPO_URL}/releases)\n"
    )


def main() -> int:
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_dir():
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    pages = {p.relative_to(WIKI).as_posix() for p in WIKI.rglob("*.md")} - {"SUMMARY.md"}
    written = set()
    for rel in sorted(pages):
        name = page_name(rel)
        body = rewrite(rel, (WIKI / rel).read_text(encoding="utf-8"), pages)
        (out / f"{name}.md").write_text(body, encoding="utf-8")
        written.add(f"{name}.md")
        print(f"{name}.md  <-  docs/wiki/{rel}")
    (out / "_Sidebar.md").write_text(sidebar(pages), encoding="utf-8")
    (out / "_Footer.md").write_text(
        f"Generated from [`docs/wiki/`]({REPO_URL}/tree/main/docs/wiki) on every push to `main` and also published "
        f"at [{SITE_URL.removeprefix('https://')}/docs]({SITE_URL}/docs). Edit the files in the repository, not here.\n",
        encoding="utf-8",
    )
    manifest = out / MANIFEST
    previous = set(manifest.read_text().split()) if manifest.exists() else set()
    for stale in sorted(previous - written):
        if (out / stale).exists():
            (out / stale).unlink()
            print(f"removed stale {stale}")
    manifest.write_text("\n".join(sorted(written)) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
