#!/usr/bin/env python3
"""Check docs/wiki: every relative link resolves to a page and every #anchor to a heading,
every page is listed in SUMMARY.md, and wiki page names are unique (stdlib only).

Anchors use GitHub's heading slugs (the website and the wiki use the same rule).
Usage: python3 -I docs/tools/check_docs.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIKI = ROOT / "docs" / "wiki"
LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```|~~~)")


def slug(text: str) -> str:
    """GitHub-style heading anchor."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links -> their text
    text = text.lower()
    text = re.sub(r"[^\w\- ]", "", text)  # keep letters, digits, _, -, space
    return text.replace(" ", "-")


def scan(path: Path) -> tuple[set[str], list[str]]:
    anchors: set[str] = set()
    links: list[str] = []
    in_code = False
    seen: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if FENCE.match(line):
            in_code = not in_code
            continue
        if in_code:
            continue
        m = HEADING.match(line)
        if m:
            s = slug(m.group(2))
            n = seen.get(s, 0)
            anchors.add(s if n == 0 else f"{s}-{n}")
            seen[s] = n + 1
        links.extend(LINK.findall(re.sub(r"`[^`]*`", "", line)))
    return anchors, links


def wiki_name(rel: str) -> str:
    sys.path.insert(0, str(Path(__file__).parent))
    from wiki_sync import page_name  # noqa: E402

    return page_name(rel)


def main() -> int:
    pages = {p.relative_to(WIKI).as_posix(): p for p in WIKI.rglob("*.md")}
    info = {rel: scan(p) for rel, p in pages.items()}
    errors: list[str] = []
    for rel, (_, links) in info.items():
        for href in links:
            if re.match(r"^[a-z]+:", href) or href.startswith("/"):
                continue
            target, _, anchor = href.partition("#")
            tgt = rel if target == "" else (Path(rel).parent / target).as_posix()
            tgt = str(Path(tgt)).replace("\\", "/")
            parts: list[str] = []
            for seg in tgt.split("/"):
                if seg == "..":
                    if parts:
                        parts.pop()
                elif seg not in ("", "."):
                    parts.append(seg)
            tgt = "/".join(parts)
            if tgt not in info:
                errors.append(f"{rel}: broken link {href}")
            elif anchor and anchor not in info[tgt][0]:
                errors.append(f"{rel}: missing anchor #{anchor} in {tgt}")
    summary = (WIKI / "SUMMARY.md").read_text(encoding="utf-8")
    listed = set(re.findall(r"\]\(([^)]+\.md)\)", summary))
    for rel in pages:
        if rel != "SUMMARY.md" and rel not in listed:
            errors.append(f"{rel}: not listed in SUMMARY.md")
    for rel in listed:
        if rel not in pages:
            errors.append(f"SUMMARY.md: lists missing page {rel}")
    names: dict[str, str] = {}
    for rel in pages:
        if rel == "SUMMARY.md":
            continue
        n = wiki_name(rel)
        if n in names:
            errors.append(f"wiki page name {n} used by {names[n]} and {rel}")
        names[n] = rel
    for e in errors:
        print(e)
    print(f"{len(pages)} pages, {sum(len(v[1]) for v in info.values())} links, {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
