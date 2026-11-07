import { useEffect, useState, type AnchorHTMLAttributes, type MouseEvent, type ReactNode } from "react";

/*
 * History-API routing with clean URLs (/install, not #/install).
 * BASE comes from Vite's `base` (VITE_BASE at build time), so the site also works under a
 * sub-path such as /arkivector/ on GitHub Pages. Static hosts need an SPA fallback; see
 * public/404.html (GitHub Pages), public/_redirects (Netlify) and public/vercel.json.
 */

export type Route = "/" | "/install" | "/architecture" | "/build" | "/next" | "/team" | "404";
export const ROUTES: Exclude<Route, "404">[] = ["/", "/install", "/architecture", "/build", "/next", "/team"];

const BASE = import.meta.env.BASE_URL.replace(/\/$/, ""); // "" or "/arkivector"

function current(): Route {
  let p = window.location.pathname;
  if (BASE && p.startsWith(BASE)) p = p.slice(BASE.length);
  p = p.replace(/\/+$/, "") || "/";
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
