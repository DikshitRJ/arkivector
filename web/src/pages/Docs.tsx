import { useEffect, useMemo, useState, type MouseEvent } from "react";
import { marked, type Token, type Tokens } from "marked";
import { Page, REPO } from "../components/Layout";
import { CodeBlock } from "../components/ui";
import { Link, navigate } from "../router";

/*
 * /docs renders docs/wiki/ (the same Markdown files are published to the GitHub wiki), so the
 * repository, the wiki and the site never drift apart. SUMMARY.md is the navigation tree.
 * Routes: README.md -> /docs, api/README.md -> /docs/api, api/search.md -> /docs/api/search.
 */
const FILES = import.meta.glob("../../../docs/wiki/**/*.md", { query: "?raw", import: "default", eager: true }) as Record<
  string,
  string
>;

const SOURCES: Record<string, string> = {};
for (const [path, text] of Object.entries(FILES)) SOURCES[path.replace(/^.*\/docs\/wiki\//, "")] = text;

export function routeOf(rel: string): string {
  const p = rel.replace(/\.md$/, "").replace(/(^|\/)README$/, "");
  return p ? `/docs/${p}` : "/docs";
}

const ROUTE_TO_FILE = new Map(Object.keys(SOURCES).map((rel) => [routeOf(rel), rel]));

export function docTitle(rel: string): string {
  const m = /^#\s+(.+)$/m.exec(SOURCES[rel] ?? "");
  return m ? m[1].replace(/`/g, "") : rel;
}

/* ------------------------------------------------------------------ navigation tree */

type NavItem = { kind: "page"; title: string; rel: string } | { kind: "section"; title: string; children: NavItem[] };

function parseSummary(md: string): NavItem[] {
  const root: NavItem[] = [];
  const stack: { indent: number; items: NavItem[] }[] = [{ indent: -1, items: root }];
  for (const line of md.split("\n")) {
    const m = /^(\s*)- (?:\[([^\]]+)\]\(([^)]+)\)|(.+))$/.exec(line);
    if (!m) continue;
    const indent = m[1].length;
    while (stack.length > 1 && indent <= stack[stack.length - 1].indent) stack.pop();
    const parent = stack[stack.length - 1].items;
    if (m[3]) {
      parent.push({ kind: "page", title: m[2], rel: m[3] });
    } else {
      const sec: NavItem = { kind: "section", title: m[4].trim(), children: [] };
      parent.push(sec);
      stack.push({ indent, items: sec.children });
    }
  }
  return root;
}

const NAV = parseSummary(SOURCES["SUMMARY.md"] ?? "");

function flatten(items: NavItem[], trail: string[] = []): { rel: string; title: string; trail: string[] }[] {
  return items.flatMap((it) =>
    it.kind === "page" ? [{ rel: it.rel, title: it.title, trail }] : flatten(it.children, [...trail, it.title]),
  );
}
const ORDER = flatten(NAV);

/* ------------------------------------------------------------------ markdown */

/** GitHub-style heading anchor (same rule as the GitHub wiki and docs/tools/check_docs.py). */
function slug(text: string): string {
  return text
    .replace(/<[^>]+>/g, "")
    .toLowerCase()
    .replace(/[^\p{L}\p{N}_\- ]/gu, "")
    .replace(/ /g, "-");
}

function decodeEntities(s: string): string {
  return s.replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&#39;/g, "'");
}

/** Resolve a link inside page `from`: sibling .md files become site routes. */
function resolveHref(from: string, href: string): { href: string; internal: boolean } {
  if (/^[a-z]+:/i.test(href) || href.startsWith("/")) return { href, internal: false };
  if (href.startsWith("#")) return { href, internal: true };
  const [target, anchor] = href.split("#");
  if (!target.endsWith(".md")) return { href, internal: false };
  const parts = from.split("/").slice(0, -1);
  for (const seg of target.split("/")) {
    if (seg === "..") parts.pop();
    else if (seg !== "." && seg !== "") parts.push(seg);
  }
  const rel = parts.join("/");
  if (!(rel in SOURCES)) return { href: `${REPO}/blob/main/docs/wiki/${rel}${anchor ? "#" + anchor : ""}`, internal: false };
  return { href: routeOf(rel) + (anchor ? "#" + anchor : ""), internal: true };
}

let currentFile = "README.md"; // renderer context (render is synchronous)

marked.use({
  gfm: true,
  renderer: {
    heading({ tokens, depth }) {
      const html = this.parser.parseInline(tokens);
      const id = slug(decodeEntities(html));
      return `<h${depth} id="${id}"><a class="doc-anchor" href="#${id}">${html}</a></h${depth}>`;
    },
    link({ href, title, tokens }) {
      const text = this.parser.parseInline(tokens);
      const r = resolveHref(currentFile, href);
      const t = title ? ` title="${title}"` : "";
      return r.internal
        ? `<a href="${r.href}"${t} data-internal="1">${text}</a>`
        : `<a href="${r.href}"${t} target="_blank" rel="noreferrer">${text}</a>`;
    },
  },
});

type Block = { kind: "html"; html: string } | { kind: "code"; code: string; lang: string };

/** Top-level code fences become <CodeBlock> (copy button); everything else is HTML. */
function render(rel: string): Block[] {
  currentFile = rel;
  const blocks: Block[] = [];
  let pending: Token[] = [];
  const flush = () => {
    if (pending.length) {
      const list = pending as Token[] & { links: Tokens.Generic["links"] };
      list.links = {};
      blocks.push({ kind: "html", html: marked.parser(list) });
      pending = [];
    }
  };
  for (const tok of marked.lexer(SOURCES[rel] ?? "")) {
    if (tok.type === "code") {
      flush();
      blocks.push({ kind: "code", code: (tok as Tokens.Code).text, lang: (tok as Tokens.Code).lang || "text" });
    } else pending.push(tok);
  }
  flush();
  return blocks;
}

function headings(rel: string): { id: string; text: string }[] {
  return marked
    .lexer(SOURCES[rel] ?? "")
    .filter((t): t is Tokens.Heading => t.type === "heading" && t.depth === 2)
    .map((h) => {
      const plain = h.text.replace(/`/g, "").replace(/\[([^\]]*)\]\([^)]*\)/g, "$1");
      return { id: slug(decodeEntities(marked.parseInline(h.text) as string)), text: plain };
    });
}

/* ------------------------------------------------------------------ sidebar */

function contains(item: NavItem, rel: string): boolean {
  return item.kind === "page" ? item.rel === rel : item.children.some((c) => contains(c, rel));
}

function FolderIcon({ open }: { open: boolean }) {
  return (
    <svg width="14" height="12" viewBox="0 0 14 12" aria-hidden className="shrink-0">
      <path d="M1 2h4l1.5 1.5H13v7.5H1z" fill={open ? "#fe7445" : "none"} stroke="#000" strokeWidth="1.2" />
    </svg>
  );
}

function PageIcon() {
  return (
    <svg width="10" height="12" viewBox="0 0 10 12" aria-hidden className="shrink-0">
      <path d="M1 1h5l3 3v7H1z" fill="none" stroke="#181ea9" strokeWidth="1.1" />
    </svg>
  );
}

function Tree({ items, active, filter, depth = 0 }: { items: NavItem[]; active: string; filter: string; depth?: number }) {
  return (
    <ul className={depth ? "ml-3 border-l border-dashed border-ink pl-2" : ""}>
      {items.map((it) =>
        it.kind === "page" ? (
          !filter || it.title.toLowerCase().includes(filter) ? (
            <li key={it.rel}>
              <Link
                to={routeOf(it.rel)}
                aria-current={it.rel === active ? "page" : undefined}
                className={`flex items-center gap-1.5 no-underline px-1.5 py-[3px] ${it.rel === active ? "bg-ink text-paper" : "hover:bg-arkiv-orange"}`}
              >
                <PageIcon />
                <span>{it.title}</span>
              </Link>
            </li>
          ) : null
        ) : (
          <Section key={it.title} item={it} active={active} filter={filter} depth={depth} />
        ),
      )}
    </ul>
  );
}

function Section({ item, active, filter, depth }: { item: Extract<NavItem, { kind: "section" }>; active: string; filter: string; depth: number }) {
  const [open, setOpen] = useState(() => contains(item, active));
  useEffect(() => {
    if (contains(item, active)) setOpen(true);
  }, [active, item]);
  const matches = !filter || flatten(item.children).some((p) => p.title.toLowerCase().includes(filter));
  if (!matches) return null;
  const shown = open || !!filter;
  return (
    <li className="mt-1">
      <button
        type="button"
        onClick={() => setOpen(!open)}
        aria-expanded={shown}
        className="flex w-full items-center gap-1.5 px-1.5 py-[3px] text-left font-semibold hover:underline underline-offset-4"
      >
        <FolderIcon open={shown} />
        <span>{item.title}</span>
      </button>
      {shown && <Tree items={item.children} active={active} filter={filter} depth={depth + 1} />}
    </li>
  );
}

/* ------------------------------------------------------------------ page */

export function Docs({ path }: { path: string }) {
  const rel = ROUTE_TO_FILE.get(path.replace(/\/+$/, "") || "/docs");
  const [filter, setFilter] = useState("");
  const [menu, setMenu] = useState(false);
  const blocks = useMemo(() => (rel ? render(rel) : []), [rel]);
  const toc = useMemo(() => (rel ? headings(rel) : []), [rel]);
  const idx = ORDER.findIndex((o) => o.rel === rel);
  const prev = idx > 0 ? ORDER[idx - 1] : null;
  const next = idx >= 0 && idx < ORDER.length - 1 ? ORDER[idx + 1] : null;
  const trail = idx >= 0 ? ORDER[idx].trail : [];

  useEffect(() => {
    document.title = rel ? `${docTitle(rel)} · Docs · Arkivector` : "Page not found · Docs · Arkivector";
    setMenu(false);
    const id = decodeURIComponent(window.location.hash.slice(1));
    if (id) requestAnimationFrame(() => document.getElementById(id)?.scrollIntoView());
  }, [rel]);

  const onClick = (e: MouseEvent<HTMLElement>) => {
    const a = (e.target as HTMLElement).closest("a");
    if (!a || a.dataset.internal !== "1" || e.metaKey || e.ctrlKey || e.shiftKey || e.button !== 0) return;
    const href = a.getAttribute("href") ?? "";
    if (href.startsWith("#")) return;
    e.preventDefault();
    const [p, hash] = href.split("#");
    navigate(p);
    if (hash) {
      window.history.replaceState({}, "", window.location.pathname + "#" + hash);
      requestAnimationFrame(() => document.getElementById(hash)?.scrollIntoView());
    }
  };

  const sidebar = (
    <nav aria-label="Documentation" className="sheet px-2.5 py-3 text-[13px]">
      <div className="flex items-center justify-between px-1.5 mb-2">
        <Link to="/docs" className="font-display font-bold text-[16px] no-underline">
          docs
        </Link>
        <span className="text-[11px] text-arkiv-blue">v1.1</span>
      </div>
      <input
        type="search"
        value={filter}
        onChange={(e) => setFilter(e.target.value.toLowerCase())}
        placeholder="filter pages…"
        aria-label="Filter pages"
        className="mb-2 w-full border-[1.5px] border-ink bg-paper px-2 py-1 text-[12.5px] outline-none focus:shadow-[2px_2px_0_#fe7445]"
      />
      <Tree items={NAV} active={rel ?? ""} filter={filter} />
      <p className="mt-4 px-1.5 text-[11.5px] leading-snug">
        Same pages on the{" "}
        <a className="ink-link" href={`${REPO}/wiki`} target="_blank" rel="noreferrer">
          GitHub wiki
        </a>
        . Source:{" "}
        <a className="ink-link" href={`${REPO}/tree/main/docs/wiki`} target="_blank" rel="noreferrer">
          docs/wiki
        </a>
        .
      </p>
    </nav>
  );

  return (
    <Page className="py-8 sm:py-12 max-w-7xl">
      <button
        type="button"
        className="lg:hidden mb-3 border-[1.5px] border-ink bg-paper px-3 py-1.5 text-[13px] shadow-[3px_3px_0_#000]"
        aria-expanded={menu}
        onClick={() => setMenu(!menu)}
      >
        {menu ? "close menu" : "☰ docs menu"}
      </button>
      <div className="grid gap-6 lg:grid-cols-[15rem_minmax(0,1fr)] xl:grid-cols-[15rem_minmax(0,1fr)_12rem]">
        <aside className={`${menu ? "block" : "hidden"} lg:block lg:sticky lg:top-4 self-start lg:max-h-[calc(100vh-2rem)] lg:overflow-y-auto`}>
          {sidebar}
        </aside>

        <div className="min-w-0">
          {rel ? (
            <>
              <p className="mb-2 text-[12px] flex flex-wrap gap-1 items-center">
                <Link to="/docs" className="ink-link">
                  docs
                </Link>
                {trail.map((t) => (
                  <span key={t}>/ {t}</span>
                ))}
                {rel !== "README.md" && <span>/ {docTitle(rel)}</span>}
              </p>
              <article className="doc-prose sheet px-4 sm:px-8 py-6" onClick={onClick}>
                {blocks.map((b, i) =>
                  b.kind === "code" ? <CodeBlock key={i} code={b.code} lang={b.lang} /> : <div key={i} dangerouslySetInnerHTML={{ __html: b.html }} />,
                )}
              </article>
              <div className="mt-4 grid grid-cols-2 gap-3 text-[13px]">
                <div>
                  {prev && (
                    <Link to={routeOf(prev.rel)} className="sheet block px-3 py-2 no-underline hover:shadow-[3px_3px_0_#fe7445]">
                      <span className="block text-[11px] text-arkiv-blue">← previous</span>
                      {prev.title}
                    </Link>
                  )}
                </div>
                <div className="text-right">
                  {next && (
                    <Link to={routeOf(next.rel)} className="sheet block px-3 py-2 no-underline hover:shadow-[3px_3px_0_#fe7445]">
                      <span className="block text-[11px] text-arkiv-blue">next →</span>
                      {next.title}
                    </Link>
                  )}
                </div>
              </div>
              <p className="mt-3 text-[12px]">
                <a className="ink-link" href={`${REPO}/edit/main/docs/wiki/${rel}`} target="_blank" rel="noreferrer">
                  edit this page on GitHub
                </a>
              </p>
            </>
          ) : (
            <article className="doc-prose sheet px-6 py-8">
              <h1>Page not found</h1>
              <p>
                There is no documentation page at <code>{path}</code>. Pick one from the menu, or start at the{" "}
                <Link to="/docs" className="ink-link">
                  documentation home
                </Link>
                .
              </p>
            </article>
          )}
        </div>

        {toc.length > 0 && (
          <aside className="hidden xl:block sticky top-4 self-start text-[12.5px]">
            <p className="font-display font-bold mb-1">on this page</p>
            <ul className="space-y-1 border-l-2 border-arkiv-orange pl-2">
              {toc.map((h) => (
                <li key={h.id}>
                  <a className="ink-link" href={`#${h.id}`}>
                    {h.text}
                  </a>
                </li>
              ))}
            </ul>
          </aside>
        )}
      </div>
    </Page>
  );
}
