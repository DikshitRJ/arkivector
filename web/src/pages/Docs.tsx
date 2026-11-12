import { useEffect, useMemo, type MouseEvent } from "react";
import { marked, type Token, type Tokens } from "marked";
import { Page, REPO } from "../components/Layout";
import { CodeBlock } from "../components/ui";
import { Link, navigate, type Route } from "../router";

/*
 * /docs renders the Markdown guides in docs/guide/ (the same files are mirrored to the GitHub
 * wiki), so the site, the repository and the wiki never drift apart. Links between guides
 * ("python.md#errors") become in-app routes ("/docs/python#errors").
 */
import overview from "../../../docs/guide/README.md?raw";
import api from "../../../docs/guide/api.md?raw";
import python from "../../../docs/guide/python.md?raw";
import node from "../../../docs/guide/node.md?raw";

export type DocRoute = Extract<Route, `/docs${string}`>;

export const DOCS: { route: DocRoute; file: string; label: string; source: string }[] = [
  { route: "/docs", file: "README.md", label: "overview", source: overview },
  { route: "/docs/api", file: "api.md", label: "REST API (curl)", source: api },
  { route: "/docs/python", file: "python.md", label: "Python SDK", source: python },
  { route: "/docs/node", file: "node.md", label: "Node.js SDK", source: node },
];

const FILE_TO_ROUTE = new Map(DOCS.map((d) => [d.file, d.route]));

function slug(text: string): string {
  return text
    .toLowerCase()
    .replace(/<[^>]+>/g, "")
    .replace(/[`*_~]/g, "")
    .replace(/[^a-z0-9\s-]/g, "")
    .trim()
    .replace(/\s+/g, "-");
}

/** Rewrite links to sibling guides into site routes; leave everything else alone. */
function rewriteHref(href: string): string {
  const m = /^([a-z]+\.md|README\.md)(#.*)?$/i.exec(href);
  if (!m) return href;
  const route = FILE_TO_ROUTE.get(m[1]);
  return route ? route + (m[2] ?? "") : href;
}

marked.use({
  gfm: true,
  renderer: {
    heading({ tokens, depth }) {
      const text = this.parser.parseInline(tokens);
      const id = slug(text);
      return `<h${depth} id="${id}"><a class="doc-anchor" href="#${id}">${text}</a></h${depth}>`;
    },
    link({ href, title, tokens }) {
      const text = this.parser.parseInline(tokens);
      const to = rewriteHref(href);
      const external = /^https?:\/\//.test(to);
      const t = title ? ` title="${title}"` : "";
      return external
        ? `<a href="${to}"${t} target="_blank" rel="noreferrer">${text}</a>`
        : `<a href="${to}"${t} data-internal="1">${text}</a>`;
    },
  },
});

type Block = { kind: "html"; html: string } | { kind: "code"; code: string; lang: string };

/** Code fences become <CodeBlock> (copy button); everything else is rendered HTML. */
function toBlocks(md: string): Block[] {
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
  for (const tok of marked.lexer(md)) {
    if (tok.type === "code") {
      flush();
      const c = tok as Tokens.Code;
      blocks.push({ kind: "code", code: c.text, lang: c.lang || "text" });
    } else {
      pending.push(tok);
    }
  }
  flush();
  return blocks;
}

function headings(md: string): { id: string; text: string }[] {
  return marked
    .lexer(md)
    .filter((t): t is Tokens.Heading => t.type === "heading" && t.depth === 2)
    .map((h) => ({ id: slug(h.text), text: h.text.replace(/`/g, "") }));
}

export function Docs({ route }: { route: DocRoute }) {
  const doc = DOCS.find((d) => d.route === route) ?? DOCS[0];
  const blocks = useMemo(() => toBlocks(doc.source), [doc]);
  const toc = useMemo(() => headings(doc.source), [doc]);

  // Jump to #anchor after navigation (the router scrolls to the top first).
  useEffect(() => {
    const id = decodeURIComponent(window.location.hash.slice(1));
    if (id) requestAnimationFrame(() => document.getElementById(id)?.scrollIntoView());
  }, [route]);

  // Client-side navigation for links between guides.
  const onClick = (e: MouseEvent<HTMLElement>) => {
    const a = (e.target as HTMLElement).closest("a");
    if (!a || a.dataset.internal !== "1" || e.metaKey || e.ctrlKey || e.shiftKey || e.button !== 0) return;
    const href = a.getAttribute("href") ?? "";
    if (href.startsWith("#")) return; // same-page anchor: let the browser scroll
    if (href.startsWith("/docs")) {
      e.preventDefault();
      const [path, hash] = href.split("#");
      navigate(path);
      if (hash) {
        window.history.replaceState({}, "", window.location.pathname + "#" + hash);
        requestAnimationFrame(() => document.getElementById(hash)?.scrollIntoView());
      }
    }
  };

  return (
    <Page className="py-10 sm:py-14">
      <div className="grid gap-6 lg:grid-cols-[13rem_minmax(0,1fr)]">
        <aside className="lg:sticky lg:top-6 self-start">
          <nav aria-label="Documentation" className="sheet px-3 py-3 text-[13.5px]">
            <p className="font-display font-bold text-[15px] mb-2">docs</p>
            <ul className="space-y-1">
              {DOCS.map((d) => (
                <li key={d.route}>
                  <Link
                    to={d.route}
                    aria-current={d.route === doc.route ? "page" : undefined}
                    className={`block no-underline px-1.5 py-0.5 ${d.route === doc.route ? "bg-ink text-paper" : "hover:underline underline-offset-4"}`}
                  >
                    {d.label}
                  </Link>
                </li>
              ))}
            </ul>
            {toc.length > 0 && (
              <div className="hidden lg:block">
                <p className="font-display font-bold text-[13px] mt-4 mb-1">on this page</p>
                <ul className="space-y-0.5 text-[12.5px]">
                  {toc.map((h) => (
                    <li key={h.id}>
                      <a className="ink-link" href={`#${h.id}`}>
                        {h.text}
                      </a>
                    </li>
                  ))}
                </ul>
              </div>
            )}
            <p className="mt-4 text-[12px]">
              <a className="ink-link" href={`${REPO}/blob/main/docs/guide/${doc.file}`} target="_blank" rel="noreferrer">
                edit on GitHub
              </a>{" "}
              ·{" "}
              <a className="ink-link" href={`${REPO}/wiki`} target="_blank" rel="noreferrer">
                wiki
              </a>
            </p>
          </nav>
        </aside>
        <article className="doc-prose sheet min-w-0 px-4 sm:px-7 py-6" onClick={onClick}>
          {blocks.map((b, i) =>
            b.kind === "code" ? (
              <CodeBlock key={i} code={b.code} lang={b.lang} />
            ) : (
              <div key={i} dangerouslySetInnerHTML={{ __html: b.html }} />
            ),
          )}
        </article>
      </div>
    </Page>
  );
}
