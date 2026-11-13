import type { ReactNode } from "react";
import { useSound } from "../audio/sound";
import { scribble } from "../ink/paths";
import { ORANGE } from "../ink/colors";
import { Link, type Route, type StaticRoute } from "../router";
import { VectorField } from "./VectorField";

export const REPO = "https://github.com/DikshitRJ/arkivector";

const NAV: { to: StaticRoute; label: string }[] = [
  { to: "/", label: "home" },
  { to: "/architecture", label: "architecture" },
  { to: "/build", label: "build" },
  { to: "/next", label: "what's next" },
  { to: "/team", label: "team" },
];

export function Logo({ size = 26 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden>
      <path d="M5 27 C 12 20, 18 13, 26.5 5.5" fill="none" stroke="#000" strokeWidth="2.6" strokeLinecap="round" />
      <path d="M18.5 6.4 L26.5 5.5 L25.6 13.4" fill="none" stroke="#000" strokeWidth="2.6" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M5 27 L5 9" fill="none" stroke="#000" strokeWidth="1.2" strokeDasharray="2 2.5" />
      <path d="M5 27 L23 27" fill="none" stroke="#000" strokeWidth="1.2" strokeDasharray="2 2.5" />
      <circle cx="5" cy="27" r="2.6" fill={ORANGE} />
    </svg>
  );
}

function GitHubMark() {
  return (
    <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden fill="#000">
      <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z" />
    </svg>
  );
}

/** Floating music control, bottom right on every page. */
function SoundToggle() {
  const { music, toggleMusic } = useSound();
  return (
    <button
      type="button"
      onClick={toggleMusic}
      aria-pressed={music}
      title={music ? "Background music on (click to mute)" : "Background music off"}
      className="fixed bottom-4 right-4 z-50 flex items-center gap-1.5 whitespace-nowrap border-[1.5px] border-ink bg-paper px-3 py-2 text-[12.5px] shadow-[3px_3px_0_#fe7445] hover:bg-ink hover:text-paper active:translate-x-[3px] active:translate-y-[3px] active:shadow-none"
    >
      <svg width="16" height="14" viewBox="0 0 16 14" aria-hidden className="shrink-0">
        <path d="M1 5h3l4-3.5v11L4 9H1z" fill="currentColor" />
        {music ? (
          <path d="M10.5 4.5c1 .9 1 4.1 0 5M12.5 2.5c2 1.9 2 7.1 0 9" fill="none" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" />
        ) : (
          <path d="M10.5 4.5l4 5M14.5 4.5l-4 5" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
        )}
      </svg>
      <span>{music ? "music on" : "music off"}</span>
    </button>
  );
}

/** Docs: a real button in the header, next to the GitHub chip; inverted while you are in the docs. */
function DocsButton({ active }: { active: boolean }) {
  return (
    <Link
      to="/docs"
      aria-current={active ? "page" : undefined}
      className={
        "flex items-center gap-1.5 whitespace-nowrap border-2 border-ink px-2 sm:px-3 py-1 text-[11px] sm:text-[12.5px] font-semibold no-underline " +
        "shadow-[2px_2px_0_#000] transition-[transform,box-shadow] duration-75 hover:-translate-x-px hover:-translate-y-px hover:shadow-[3px_3px_0_#000] " +
        "active:translate-x-[2px] active:translate-y-[2px] active:shadow-none " +
        (active ? "bg-ink text-paper" : "bg-paper text-ink")
      }
    >
      <svg width="13" height="14" viewBox="0 0 13 14" aria-hidden className="shrink-0">
        <path d="M1.5 1.5h7l3 3v8h-10z" fill="none" stroke="currentColor" strokeWidth="1.4" />
        <path d="M4 7h5M4 9.5h5" stroke="currentColor" strokeWidth="1.2" />
      </svg>
      <span>docs</span>
    </Link>
  );
}

function RepoChip() {
  return (
    <a
      href={REPO}
      target="_blank"
      rel="noreferrer"
      className="flex items-center gap-1.5 sm:gap-2 whitespace-nowrap border-2 border-arkiv-orange bg-paper px-1.5 sm:px-2.5 py-1 text-[11px] sm:text-[12.5px] font-semibold shadow-[2px_2px_0_#fe7445] hover:bg-arkiv-orange"
    >
      <GitHubMark />
      <span>[ DikshitRJ/arkivector ]</span>
    </a>
  );
}

export function Layout({ route, children }: { route: Route; children: ReactNode }) {
  return (
    <div className="min-h-screen flex flex-col">
      <VectorField />
      <SoundToggle />
      <header className="bg-paper border-b-[1.5px] border-ink">
        {/* phones: logo + chip on one row, links scroll below; lg: 3 equal-sided columns so the links sit dead centre */}
        <div className="px-4 sm:px-6 pt-3 pb-2 lg:pb-3 flex flex-wrap items-center gap-x-2 sm:gap-x-6 gap-y-2 lg:grid lg:grid-cols-[1fr_auto_1fr]">
          <Link to="/" className="flex items-center gap-2 no-underline" aria-label="Arkivector home">
            <Logo />
            <span className="font-display text-[19px] font-bold tracking-tight">arkivector</span>
          </Link>
          <div className="ml-auto lg:ml-0 lg:justify-self-end lg:order-3 flex items-center gap-2 sm:gap-3">
            <DocsButton active={route.startsWith("/docs")} />
            <RepoChip />
          </div>
          <nav
            aria-label="Main"
            className="order-last lg:order-2 w-full lg:w-auto lg:mx-0 -mx-1 px-1 flex items-center gap-4 sm:gap-5 text-[13.5px] overflow-x-auto lg:overflow-visible"
          >
            {NAV.map((n) => {
              const active = route === n.to;
              return (
                <Link
                  key={n.to}
                  to={n.to}
                  aria-current={active ? "page" : undefined}
                  className={`relative whitespace-nowrap no-underline py-1.5 ${active ? "font-semibold" : "hover:underline underline-offset-4"}`}
                >
                  {n.label}
                  {active && (
                    <svg aria-hidden className="pointer-events-none absolute left-0 bottom-0 w-full h-2" viewBox="0 0 100 8" preserveAspectRatio="none">
                      <path d={scribble(1, 4, 98, 3)} fill="none" stroke={ORANGE} strokeWidth={2.4} strokeLinecap="round" vectorEffect="non-scaling-stroke" />
                    </svg>
                  )}
                </Link>
              );
            })}
          </nav>
        </div>
      </header>
      <main className="flex-1">{children}</main>
      <footer className="bg-paper border-t-[1.5px] border-ink">
        <div className="mx-auto max-w-5xl px-4 sm:px-6 pt-6 pb-20 flex flex-col sm:flex-row gap-3 sm:items-center sm:justify-between text-[13px]">
          <p className="flex flex-wrap gap-x-4 gap-y-1">
            <span>Built for ETHGlobal Mumbai 2026.</span>
            <a className="ink-link" href={REPO} target="_blank" rel="noreferrer">source</a>
            <a className="ink-link" href={`${REPO}/releases`} target="_blank" rel="noreferrer">releases</a>
            <a className="ink-link" href="https://pypi.org/project/arkivector/" target="_blank" rel="noreferrer">pypi</a>
          </p>
          <p className="sm:text-right">
            © 2026 DikshitRJ ·{" "}
            <a className="ink-link" href={`${REPO}/blob/main/LICENSE`} target="_blank" rel="noreferrer">
              MIT License
            </a>
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
