/*
 * Keep the head in step with client-side navigation. Each route also has its own prerendered
 * HTML file (plugins/site-files.ts), so crawlers and link previews get the right tags without JS.
 */
const SITE = "https://arkivector.dikshitrj.in";

function meta(selector: string, attr: "name" | "property", key: string): HTMLMetaElement {
  let el = document.head.querySelector<HTMLMetaElement>(selector);
  if (!el) {
    el = document.createElement("meta");
    el.setAttribute(attr, key);
    document.head.appendChild(el);
  }
  return el;
}

/** Title, canonical URL, Markdown alternate and robots for the current route. */
export function setHead(title: string, path: string, opts: { notFound?: boolean } = {}): void {
  document.title = title;
  const url = SITE + (path === "/" ? "/" : path.replace(/\/+$/, ""));
  document.head.querySelector<HTMLLinkElement>('link[rel="canonical"]')?.setAttribute("href", url);
  meta('meta[property="og:url"]', "property", "og:url").content = url;
  meta('meta[property="og:title"]', "property", "og:title").content = title;
  meta('meta[name="twitter:title"]', "name", "twitter:title").content = title;
  const md = path === "/" ? "/agents.md" : path === "/docs" ? "/docs/index.md" : path.startsWith("/docs/") ? `${path}.md` : "/agents.md";
  document.head.querySelector<HTMLLinkElement>('link[rel="alternate"][type="text/markdown"]')?.setAttribute("href", SITE + md);
  meta('meta[name="robots"]', "name", "robots").content = opts.notFound
    ? "noindex, follow"
    : "index, follow, max-image-preview:large, max-snippet:-1";
}
