import type { ReactNode } from "react";
import { Link, type Route } from "../router";
import { Underline } from "../ink/Ink";

export const REPO = "https://github.com/DikshitRJ/arkivector";

const NAV: { to: Route; label: string; w: number }[] = [
  { to: "/", label: "home", w: 38 },
  { to: "/install", label: "install", w: 62 },
  { to: "/architecture", label: "architecture", w: 104 },
  { to: "/team", label: "team", w: 36 },
];

export function Logo({ size = 26 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden>
      <path d="M5 27 C 12 20, 18 13, 26.5 5.5" fill="none" stroke="#000" strokeWidth="2.6" strokeLinecap="round" />
      <path d="M18.5 6.4 L26.5 5.5 L25.6 13.4" fill="none" stroke="#000" strokeWidth="2.6" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M5 27 L5 9" fill="none" stroke="#000" strokeWidth="1.2" strokeDasharray="2 2.5" />
      <path d="M5 27 L23 27" fill="none" stroke="#000" strokeWidth="1.2" strokeDasharray="2 2.5" />
      <circle cx="5" cy="27" r="2.4" fill="#000" />
    </svg>
  );
}

export function Layout({ route, children }: { route: Route; children: ReactNode }) {
  return (
    <div className="min-h-screen flex flex-col">
      <header className="bg-paper border-b-[1.5px] border-ink">
        <div className="mx-auto max-w-5xl px-4 sm:px-6 py-3 flex flex-wrap items-center justify-between gap-x-6 gap-y-2">
          <Link to="/" className="flex items-center gap-2 no-underline" aria-label="Arkivector home">
            <Logo />
            <span className="font-display text-[19px] font-bold tracking-tight">arkivector</span>
          </Link>
          <nav aria-label="Main" className="flex items-center gap-4 sm:gap-6 text-[13.5px]">
            {NAV.map((n) => {
              const active = route === n.to;
              return (
                <Link
                  key={n.to}
                  to={n.to}
                  aria-current={active ? "page" : undefined}
                  className={`relative whitespace-nowrap no-underline py-1 ${active ? "font-semibold" : "hover:underline underline-offset-4"}`}
                >
                  {n.label}
                  {active && <Underline width={n.w} />}
                </Link>
              );
            })}
            <a href={REPO} className="whitespace-nowrap py-1 hover:underline underline-offset-4" target="_blank" rel="noreferrer">
              github&nbsp;&#8599;
            </a>
          </nav>
        </div>
      </header>
      <main className="flex-1">{children}</main>
      <footer className="bg-paper border-t-[1.5px] border-ink">
        <div className="mx-auto max-w-5xl px-4 sm:px-6 py-6 flex flex-col sm:flex-row gap-2 sm:items-center sm:justify-between text-[13px]">
          <p>
            Arkivector, MIT licensed. Built for ETHGlobal Mumbai 2026.
          </p>
          <p className="flex gap-4">
            <a className="ink-link" href={REPO} target="_blank" rel="noreferrer">source</a>
            <a className="ink-link" href={`${REPO}/releases`} target="_blank" rel="noreferrer">releases</a>
            <a className="ink-link" href="https://pypi.org/project/arkivector/" target="_blank" rel="noreferrer">pypi</a>
          </p>
        </div>
      </footer>
    </div>
  );
}

/** Standard page width + gutters. */
export function Page({ children, className = "" }: { children: ReactNode; className?: string }) {
  return <div className={`mx-auto max-w-5xl px-4 sm:px-6 ${className}`}>{children}</div>;
}
