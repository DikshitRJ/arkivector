#!/usr/bin/env python3
"""Render docs/guide/*.md as GitHub wiki pages (stdlib only).

The guides in docs/guide/ are the single source for the README links, the website's /docs
section and the GitHub wiki. This script copies them into a checked-out wiki repository with
wiki page names, rewrites links between guides to wiki links, and writes _Sidebar.md and
_Footer.md. It never deletes pages it does not own.

Usage: python3 -I docs/tools/wiki_sync.py <wiki-checkout-dir>
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUIDE = ROOT / "docs" / "guide"
REPO_URL = "https://github.com/DikshitRJ/arkivector"

# guide file -> wiki page name (order = sidebar order)
PAGES: list[tuple[str, str, str]] = [
    ("README.md", "Home", "Overview"),
    ("api.md", "REST-API", "REST API (curl)"),
    ("python.md", "Python-SDK", "Python SDK"),
    ("node.md", "Node.js-SDK", "Node.js SDK"),
    ("mcp.md", "MCP-Server", "MCP server"),
]

LINK = re.compile(r"\]\(([A-Za-z0-9_.-]+\.md)(#[^)]*)?\)")


def rewrite(text: str, names: dict[str, str]) -> str:
    def sub(m: re.Match[str]) -> str:
        page = names.get(m.group(1))
        if page is None:  # a repository file, not a guide: link to GitHub
            return f"]({REPO_URL}/blob/main/docs/guide/{m.group(1)}{m.group(2) or ''})"
        return f"]({page}{m.group(2) or ''})"

    return LINK.sub(sub, text)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    if not out.is_dir():
        print(f"not a directory: {out}", file=sys.stderr)
        return 2
    present = [(f, p, t) for f, p, t in PAGES if (GUIDE / f).exists()]
    names = {f: p for f, p, _ in present}
    for f, page, _ in present:
        body = rewrite((GUIDE / f).read_text(encoding="utf-8"), names)
        (out / f"{page}.md").write_text(body, encoding="utf-8")
        print(f"wrote {page}.md from docs/guide/{f}")
    sidebar = ["**Arkivector**", ""] + [f"- [{t}]({p})" for _, p, t in present]
    sidebar += ["", f"[Repository]({REPO_URL}) · [Releases]({REPO_URL}/releases)", ""]
    (out / "_Sidebar.md").write_text("\n".join(sidebar), encoding="utf-8")
    (out / "_Footer.md").write_text(
        f"Generated from [`docs/guide/`]({REPO_URL}/tree/main/docs/guide) on every push to `main`. "
        "Edit the guides there, not here.\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
