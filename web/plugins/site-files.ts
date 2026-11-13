/*
 * Build-time files for search engines and AI agents, generated from the same sources as the site:
 *
 *   /agents.md          the whole website as Markdown (content/site.md + a docs index)
 *   /docs.md            every docs/wiki page on one Markdown page, in SUMMARY.md order
 *   /llms.txt           llmstxt.org index; /llms-full.txt = agents.md + docs.md
 *   /docs/<page>.md     each docs page as Markdown (the docs home is /docs/index.md)
 *   /<route>.html       one HTML entry per route with its own title, description, canonical URL,
 *                       Open Graph/Twitter tags and JSON-LD (Cloudflare serves /x from x.html)
 *   /sitemap.xml, /robots.txt, /site.webmanifest
 *
 * index.html marks the per-route head block with <!--seo:start--> ... <!--seo:end-->.
 */
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import type { Plugin } from "vite";

export const SITE = "https://arkivector.dikshitrj.in";
const REPO = "https://github.com/DikshitRJ/arkivector";

type Page = { route: string; title: string; description: string; markdown: string; file: string; type: "website" | "article"; trail: string[] };

const STATIC: Omit<Page, "markdown">[] = [
  {
    route: "/",
    title: "Arkivector: vector search for Arkiv",
    description:
      "pgvector's similarity search for data stored on the Arkiv chain. A C11 daemon, Python and Node.js SDKs, and an MCP server for AI agents.",
    file: "web/src/pages/Home.tsx",
    type: "website",
    trail: [],
  },
  {
    route: "/architecture",
    title: "Architecture · Arkivector",
    description:
      "How Arkivector works: a write, a sync and a search followed through the daemon, VectorBytes on chain, halfvec encoding, exact scan and HNSW.",
    file: "web/src/pages/Architecture.tsx",
    type: "website",
    trail: [],
  },
  {
    route: "/build",
    title: "Build artifacts · Arkivector",
    description:
      "Where every Arkivector release artifact is published: the PyPI and npm SDKs, the arkivectord container image and Linux binary, and the Arkiv Rabdi testnet.",
    file: "web/src/pages/Build.tsx",
    type: "website",
    trail: [],
  },
  {
    route: "/next",
    title: "What's next · Arkivector",
    description:
      "The Arkivector roadmap: persistent HNSW graphs, user-wallet signing, dynamic gas fees, Qdrant writes, mainnet, macOS and Windows, more SDKs.",
    file: "web/src/pages/Next.tsx",
    type: "website",
    trail: [],
  },
  {
    route: "/team",
    title: "Team · Arkivector",
    description: "Who built Arkivector, vector search for the Arkiv chain, at ETHGlobal Mumbai 2026.",
    file: "web/src/pages/Team.tsx",
    type: "website",
    trail: [],
  },
];

/* ------------------------------------------------------------------ helpers */

const esc = (s: string) => s.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

export function routeOf(rel: string): string {
  const p = rel.replace(/\.md$/, "").replace(/(^|\/)README$/, "");
  return p ? `/docs/${p}` : "/docs";
}
const mdUrlOf = (route: string) => (route === "/docs" ? "/docs/index.md" : route === "/" ? "/agents.md" : `${route}.md`);
const htmlFileOf = (route: string) => (route === "/" ? "index.html" : `${route.slice(1)}.html`);

function walk(dir: string): string[] {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    return statSync(p).isDirectory() ? walk(p) : p.endsWith(".md") ? [p] : [];
  });
}

/** SUMMARY.md -> pages in reading order, each with its section trail. */
function readSummary(md: string): { rel: string; title: string; trail: string[] }[] {
  const out: { rel: string; title: string; trail: string[] }[] = [];
  const stack: { indent: number; title: string }[] = [];
  for (const line of md.split("\n")) {
    const m = /^(\s*)- (?:\[([^\]]+)\]\(([^)]+)\)|(.+))$/.exec(line);
    if (!m) continue;
    const indent = m[1].length;
    while (stack.length && indent <= stack[stack.length - 1].indent) stack.pop();
    if (m[3]) out.push({ rel: m[3], title: m[2], trail: stack.map((s) => s.title) });
    else stack.push({ indent, title: m[4].trim() });
  }
  return out;
}

/** Rewrite relative links in a docs page to absolute site URLs (outside code fences). */
function absolutize(md: string, from: string, known: Set<string>): string {
  let fence = false;
  return md
    .split("\n")
    .map((line) => {
      if (/^\s*(```|~~~)/.test(line)) fence = !fence;
      if (fence) return line;
      return line.replace(/\]\(([^)\s]+)((?:\s+"[^"]*")?)\)/g, (all, href: string, title: string) => {
        if (/^[a-z]+:/i.test(href) || href.startsWith("#")) return all;
        if (href.startsWith("/")) return `](${SITE}${href}${title})`;
        const [target, anchor] = href.split("#");
        const parts = from.split("/").slice(0, -1);
        for (const seg of target.split("/")) {
          if (seg === "..") parts.pop();
          else if (seg !== "." && seg !== "") parts.push(seg);
        }
        const rel = parts.join("/");
        const hash = anchor ? `#${anchor}` : "";
        if (known.has(rel)) return `](${SITE}${routeOf(rel)}${hash}${title})`;
        return `](${REPO}/blob/main/docs/wiki/${rel}${hash}${title})`;
      });
    })
    .join("\n");
}

/** Shift every heading down by `by` levels (outside code fences). */
function demote(md: string, by: number): string {
  let fence = false;
  return md
    .split("\n")
    .map((line) => {
      if (/^\s*(```|~~~)/.test(line)) fence = !fence;
      if (fence) return line;
      const m = /^(#{1,6})(\s.*)$/.exec(line);
      return m ? "#".repeat(Math.min(6, m[1].length + by)) + m[2] : line;
    })
    .join("\n");
}

/** First prose paragraph as plain text, cut to ~155 characters. */
function summarize(md: string): string {
  let fence = false;
  const paras: string[] = [];
  let cur: string[] = [];
  for (const line of md.split("\n")) {
    if (/^\s*(```|~~~)/.test(line)) {
      fence = !fence;
      continue;
    }
    if (fence) continue;
    if (!line.trim() || /^(#|\||>|-{3,}|\s*[-*] |\s*\d+\. )/.test(line)) {
      if (cur.length) paras.push(cur.join(" "));
      cur = [];
      continue;
    }
    cur.push(line.trim());
  }
  if (cur.length) paras.push(cur.join(" "));
  let first = "";
  for (const para of paras) {
    first = first ? `${first} ${para}` : para;
    if (first.length >= 90) break;
  }
  const text = first
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1")
    .replace(/[`*]/g, "")
    .replace(/<[^>]+>/g, "")
    .replace(/\s+/g, " ")
    .trim();
  if (text.length <= 158) return text;
  const cut = text.slice(0, 155);
  return cut.slice(0, cut.lastIndexOf(" ")).replace(/[,;:]$/, "") + "…";
}

function gitDate(root: string, path: string, fallback: string): string {
  try {
    const d = execFileSync("git", ["log", "-1", "--format=%cs", "--", path], { cwd: root, encoding: "utf8" }).trim();
    return d || fallback;
  } catch {
    return fallback;
  }
}

/* ------------------------------------------------------------------ the plugin */

export function siteFiles(): Plugin {
  const webDir = fileURLToPath(new URL("..", import.meta.url));
  const repo = resolve(webDir, "..");
  const wiki = join(repo, "docs/wiki");
  let outDir = join(webDir, "dist");

  return {
    name: "arkivector-site-files",
    apply: "build",
    configResolved(c) {
      outDir = resolve(c.root, c.build.outDir);
    },
    closeBundle() {
      const today = new Date().toISOString().slice(0, 10);
      const version = /^## (\d+\.\d+\.\d+)/m.exec(readFileSync(join(repo, "CHANGELOG.md"), "utf8"))?.[1] ?? "1.1.0";

      // docs pages
      const sources = new Map(walk(wiki).map((p) => [relative(wiki, p).split("\\").join("/"), readFileSync(p, "utf8")]));
      const known = new Set(sources.keys());
      const order = readSummary(sources.get("SUMMARY.md") ?? "").filter((o) => sources.has(o.rel));
      const docs: Page[] = order.map(({ rel, trail }) => {
        const md = sources.get(rel)!;
        const h1 = /^#\s+(.+)$/m.exec(md)?.[1].replace(/`/g, "") ?? rel;
        return {
          route: routeOf(rel),
          title: `${h1} · Docs · Arkivector`,
          description: summarize(md.replace(/^#\s+.+$/m, "")) || `${h1}: Arkivector documentation.`,
          markdown: absolutize(md, rel, known).trim(),
          file: `docs/wiki/${rel}`,
          type: "article",
          trail,
        };
      });
      const h1Of = (p: Page) => p.title.replace(/ · Docs · Arkivector$/, "");

      // per-page markdown mirrors
      for (const p of docs) {
        const out = join(outDir, mdUrlOf(p.route).slice(1));
        mkdirSync(dirname(out), { recursive: true });
        writeFileSync(out, `${p.markdown}\n\n---\nWeb page: ${SITE}${p.route} · All docs on one page: ${SITE}/docs.md · Whole site: ${SITE}/agents.md\n`);
      }

      // docs.md
      const toc = docs
        .map((p) => `${"  ".repeat(p.trail.length)}- [${h1Of(p)}](${SITE}${p.route})`)
        .join("\n");
      const docsMd = [
        `# Arkivector documentation (v${version})`,
        "",
        `> Every page of the Arkivector documentation on one Markdown page, for AI agents and LLMs. Generated from docs/wiki in ${REPO} (the same files as ${SITE}/docs and the GitHub wiki). The website itself, as Markdown: ${SITE}/agents.md.`,
        "",
        "## Contents",
        "",
        toc,
        "",
        ...docs.flatMap((p) => ["---", "", `<!-- page: ${SITE}${p.route} -->`, "", demote(p.markdown, 1), ""]),
      ].join("\n");
      writeFileSync(join(outDir, "docs.md"), docsMd);

      // agents.md
      let section = "";
      const index = docs
        .map((p) => {
          const head = p.trail.join(" / ") || "Start here";
          const line = `- [${h1Of(p)}](${SITE}${p.route}) ([Markdown](${SITE}${mdUrlOf(p.route)})): ${p.description}`;
          if (head === section) return line;
          section = head;
          return `\n### ${head}\n\n${line}`;
        })
        .join("\n")
        .trim();
      const fill = (s: string) => s.replace(/\{\{SITE\}\}/g, SITE).replace(/\{\{VERSION\}\}/g, version);
      const siteMd = fill(readFileSync(join(webDir, "content/site.md"), "utf8"));
      writeFileSync(join(outDir, "agents.md"), siteMd.replace("{{DOCS_INDEX}}", index));
      writeFileSync(join(outDir, "llms-full.txt"), siteMd.replace("{{DOCS_INDEX}}", "") + "\n\n" + docsMd);

      // llms.txt
      writeFileSync(
        join(outDir, "llms.txt"),
        [
          "# Arkivector",
          "",
          "> Vector search for the Arkiv chain: pgvector's distance functions over entities stored on Arkiv. A C11 daemon (arkivectord) with a REST API and an MCP server, plus Python and Node.js SDKs.",
          "",
          `Current release: v${version}. Source: ${REPO}`,
          "",
          "## Start here",
          "",
          `- [The whole website as Markdown](${SITE}/agents.md): what Arkivector is, how it works, artifacts, roadmap`,
          `- [All documentation on one page](${SITE}/docs.md): install, REST API, SDKs, MCP, operations`,
          `- [Install and get started](${SITE}/docs/install.md): daemon, wallet, access key, SDKs`,
          "",
          "## Docs",
          "",
          ...docs.map((p) => `- [${h1Of(p)}](${SITE}${mdUrlOf(p.route)}): ${p.description}`),
          "",
          "## Optional",
          "",
          `- [Everything in one file](${SITE}/llms-full.txt)`,
          `- [Source code](${REPO})`,
          "",
        ].join("\n"),
      );

      // per-route HTML
      const template = readFileSync(join(outDir, "index.html"), "utf8");
      if (!template.includes("<!--seo:start-->")) throw new Error("index.html is missing the <!--seo:start--> marker");
      const pages: Page[] = [...STATIC.map((p) => ({ ...p, markdown: "" })), ...docs];
      for (const p of pages) {
        const url = SITE + (p.route === "/" ? "/" : p.route);
        const ld: object[] =
          p.route === "/"
            ? [
                { "@context": "https://schema.org", "@type": "WebSite", name: "Arkivector", url: `${SITE}/`, description: p.description },
                {
                  "@context": "https://schema.org",
                  "@type": "SoftwareApplication",
                  name: "Arkivector",
                  applicationCategory: "DeveloperApplication",
                  operatingSystem: "Linux, Docker",
                  softwareVersion: version,
                  description: p.description,
                  url: `${SITE}/`,
                  downloadUrl: `${REPO}/releases`,
                  codeRepository: REPO,
                  image: `${SITE}/poster.png`,
                  offers: { "@type": "Offer", price: "0", priceCurrency: "USD" },
                  author: { "@type": "Person", name: "Dikshit Rishi Jain", url: "https://github.com/DikshitRJ" },
                },
              ]
            : p.type === "article"
              ? [
                  {
                    "@context": "https://schema.org",
                    "@type": "TechArticle",
                    headline: h1Of(p),
                    description: p.description,
                    url,
                    image: `${SITE}/poster.png`,
                    dateModified: gitDate(repo, p.file, today),
                    isPartOf: { "@type": "WebSite", name: "Arkivector", url: `${SITE}/` },
                  },
                  {
                    "@context": "https://schema.org",
                    "@type": "BreadcrumbList",
                    itemListElement: [
                      { name: "Arkivector", item: `${SITE}/` },
                      { name: "Docs", item: `${SITE}/docs` },
                      ...(p.route === "/docs" ? [] : [{ name: h1Of(p), item: url }]),
                    ].map((e, i) => ({ "@type": "ListItem", position: i + 1, ...e })),
                  },
                ]
              : [
                  {
                    "@context": "https://schema.org",
                    "@type": "BreadcrumbList",
                    itemListElement: [
                      { "@type": "ListItem", position: 1, name: "Arkivector", item: `${SITE}/` },
                      { "@type": "ListItem", position: 2, name: p.title.replace(/ · Arkivector$/, ""), item: url },
                    ],
                  },
                ];
        const head = [
          `<title>${esc(p.title)}</title>`,
          `<meta name="description" content="${esc(p.description)}" />`,
          `<link rel="canonical" href="${url}" />`,
          `<link rel="alternate" type="text/markdown" href="${SITE}${mdUrlOf(p.route)}" title="This page as Markdown" />`,
          `<meta property="og:type" content="${p.type}" />`,
          `<meta property="og:title" content="${esc(p.title)}" />`,
          `<meta property="og:description" content="${esc(p.description)}" />`,
          `<meta property="og:url" content="${url}" />`,
          `<meta name="twitter:title" content="${esc(p.title)}" />`,
          `<meta name="twitter:description" content="${esc(p.description)}" />`,
          `<script type="application/ld+json">${JSON.stringify(ld.length === 1 ? ld[0] : ld).replace(/</g, "\\u003c")}</script>`,
        ].join("\n    ");
        const html = template.replace(/<!--seo:start-->[\s\S]*?<!--seo:end-->/, `<!--seo:start-->\n    ${head}\n    <!--seo:end-->`);
        const out = join(outDir, htmlFileOf(p.route));
        mkdirSync(dirname(out), { recursive: true });
        writeFileSync(out, html);
      }

      // sitemap.xml
      writeFileSync(
        join(outDir, "sitemap.xml"),
        [
          '<?xml version="1.0" encoding="UTF-8"?>',
          '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">',
          ...pages.map(
            (p) => `  <url><loc>${SITE}${p.route === "/" ? "/" : p.route}</loc><lastmod>${gitDate(repo, p.file, today)}</lastmod></url>`,
          ),
          "</urlset>",
          "",
        ].join("\n"),
      );

      // robots.txt
      writeFileSync(
        join(outDir, "robots.txt"),
        [
          "# Arkivector: vector search for Arkiv",
          "#",
          "# AI agents and LLMs: this site is also available as Markdown, written for you.",
          `#   Whole website:          ${SITE}/agents.md`,
          `#   All docs on one page:   ${SITE}/docs.md`,
          `#   llms.txt index:         ${SITE}/llms.txt`,
          `#   Any docs page:          add .md to its URL (docs home: ${SITE}/docs/index.md)`,
          "",
          "User-agent: *",
          "Allow: /",
          "Disallow: /404.html",
          "",
          `Sitemap: ${SITE}/sitemap.xml`,
          "",
        ].join("\n"),
      );

      // site.webmanifest
      writeFileSync(
        join(outDir, "site.webmanifest"),
        JSON.stringify(
          {
            name: "Arkivector",
            short_name: "Arkivector",
            description: STATIC[0].description,
            start_url: "/",
            display: "standalone",
            background_color: "#ffffff",
            theme_color: "#ffffff",
            icons: [
              { src: "/icon-192.png", sizes: "192x192", type: "image/png" },
              { src: "/icon-512.png", sizes: "512x512", type: "image/png" },
            ],
          },
          null,
          2,
        ) + "\n",
      );

      console.log(`site files: ${pages.length} routes, ${docs.length} docs pages, agents.md, docs.md, llms.txt, sitemap.xml, robots.txt`);
    },
  };
}
