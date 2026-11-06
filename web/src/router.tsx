import { useEffect, useState, type AnchorHTMLAttributes, type ReactNode } from "react";

export type Route = "/" | "/install" | "/architecture" | "/team";
const ROUTES: Route[] = ["/", "/install", "/architecture", "/team"];

function current(): Route {
  const h = window.location.hash.replace(/^#/, "") || "/";
  return (ROUTES as string[]).includes(h) ? (h as Route) : "/";
}

/** Hash routing (#/install): works on any static host, no server rewrites needed. */
export function useRoute(): Route {
  const [route, setRoute] = useState<Route>(current);
  useEffect(() => {
    const onHash = () => {
      setRoute(current());
      window.scrollTo({ top: 0 });
    };
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);
  return route;
}

type LinkProps = { to: Route; children: ReactNode } & Omit<AnchorHTMLAttributes<HTMLAnchorElement>, "href">;

export function Link({ to, children, ...rest }: LinkProps) {
  return (
    <a href={`#${to}`} {...rest}>
      {children}
    </a>
  );
}
