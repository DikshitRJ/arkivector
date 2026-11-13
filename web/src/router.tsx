import { useEffect, useState, type AnchorHTMLAttributes, type MouseEvent, type ReactNode } from "react";

/*
 * History-API routing with clean URLs (/docs, not #/docs).
 * BASE comes from Vite's `base` (VITE_BASE at build time), so the site also works under a
 * sub-path such as /arkivector/ on GitHub Pages. Static hosts need an SPA fallback: on
 * Cloudflare Workers that is `not_found_handling` in wrangler.jsonc; public/404.html covers
 * GitHub Pages.
 */

export type StaticRoute = "/" | "/architecture" | "/build" | "/team";
/** Docs pages are /docs and /docs/<folder>/<page> (resolved against docs/wiki by the Docs page). */
export type Route = StaticRoute | "/docs" | `/docs/${string}` | "404";
export const ROUTES: StaticRoute[] = ["/", "/architecture", "/build", "/team"];

/** Old addresses kept working: the install page moved into the docs; the roadmap page was retired. */
const MOVED: Record<string, string> = { "/install": "/docs/install", "/next": "/" };

const BASE = import.meta.env.BASE_URL.replace(/\/$/, ""); // "" or "/arkivector"

function current(): Route {
  let p = window.location.pathname;
  if (BASE && p.startsWith(BASE)) p = p.slice(BASE.length);
  p = p.replace(/\/+$/, "") || "/";
  if (MOVED[p]) {
    p = MOVED[p];
    window.history.replaceState({}, "", BASE + p + window.location.hash);
  }
  if (p === "/docs" || p.startsWith("/docs/")) return p as Route;
  return (ROUTES as string[]).includes(p) ? (p as Route) : "404";
}

const listeners = new Set<() => void>();

export function navigate(to: string) {
  const url = BASE + (to === "/" ? "/" : to);
  if (url !== window.location.pathname) {
    window.history.pushState({}, "", url);
    listeners.forEach((l) => l());
  }
  window.scrollTo({ top: 0 });
}

export function useRoute(): Route {
  const [route, setRoute] = useState<Route>(current);
  useEffect(() => {
    // GitHub Pages fallback: 404.html stores the requested path, index.html restores it.
    const redirect = sessionStorage.getItem("spa-redirect");
    if (redirect) {
      sessionStorage.removeItem("spa-redirect");
      window.history.replaceState({}, "", redirect);
    }
    const on = () => setRoute(current());
    on();
    listeners.add(on);
    window.addEventListener("popstate", on);
    return () => {
      listeners.delete(on);
      window.removeEventListener("popstate", on);
    };
  }, []);
  return route;
}

export const href = (to: string) => BASE + (to === "/" ? "/" : to);

type LinkProps = { to: string; children: ReactNode } & Omit<AnchorHTMLAttributes<HTMLAnchorElement>, "href">;

/** In-app link: real href for new tabs and crawlers, client-side navigation on plain clicks. */
export function Link({ to, children, onClick, ...rest }: LinkProps) {
  const go = (e: MouseEvent<HTMLAnchorElement>) => {
    onClick?.(e);
    if (e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    e.preventDefault();
    navigate(to);
  };
  return (
    <a href={href(to)} onClick={go} {...rest}>
      {children}
    </a>
  );
}
