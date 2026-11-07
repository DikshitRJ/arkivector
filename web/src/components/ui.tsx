import { useState, type ReactNode } from "react";
import { Link } from "../router";

/** Solid ink button with a hard offset shadow that presses flat when clicked. */
export function Button({
  href,
  children,
  variant = "solid",
  external = false,
}: {
  href: string;
  children: ReactNode;
  variant?: "solid" | "outline";
  external?: boolean;
}) {
  const base =
    "inline-flex items-center gap-2 whitespace-nowrap border-[1.5px] border-ink px-4 py-2 text-[14px] font-medium no-underline " +
    "shadow-[3px_3px_0_#000] transition-[transform,box-shadow] duration-75 " +
    "hover:-translate-x-px hover:-translate-y-px hover:shadow-[4px_4px_0_#000] " +
    "active:translate-x-[3px] active:translate-y-[3px] active:shadow-none";
  const look = variant === "solid" ? "bg-ink text-paper" : "bg-paper text-ink";
  if (!external && href.startsWith("/")) {
    return (
      <Link to={href} className={`${base} ${look}`}>
        {children}
      </Link>
    );
  }
  return (
    <a href={href} className={`${base} ${look}`} {...(external ? { target: "_blank", rel: "noreferrer" } : {})}>
      {children}
    </a>
  );
}

/** Code on a paper card with a copy button. Comment lines are set apart by a dotted rule. */
export function CodeBlock({ code, lang, title }: { code: string; lang: string; title?: string }) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(code);
      setCopied(true);
      setTimeout(() => setCopied(false), 1400);
    } catch {
      /* clipboard blocked: the text is still selectable */
    }
  };
  return (
    <figure className="my-4 min-w-0 bg-paper border-[1.5px] border-arkiv-blue shadow-[3px_3px_0_#181ea9]">
      <figcaption className="flex items-center justify-between border-b border-dashed border-arkiv-blue px-3 py-1.5 text-[12.5px]">
        <span className="text-arkiv-blue font-semibold">{title ?? lang}</span>
        <button
          type="button"
          onClick={copy}
          className={`whitespace-nowrap border border-arkiv-blue px-2 py-0.5 ${copied ? "bg-arkiv-orange text-ink border-ink" : "text-arkiv-blue hover:bg-arkiv-blue hover:text-paper"}`}
          aria-live="polite"
        >
          {copied ? "copied" : "copy"}
        </button>
      </figcaption>
      <pre className="overflow-x-auto px-3 py-3 text-[13px] leading-[1.6]">
        <code>{code}</code>
      </pre>
    </figure>
  );
}

/** Folder-style tabs: the active tab joins its panel; inactive tabs are hatched. */
export function Tabs({ tabs }: { tabs: { id: string; label: string; body: ReactNode }[] }) {
  const [active, setActive] = useState(tabs[0].id);
  return (
    <div className="my-4">
      <div role="tablist" className="flex flex-wrap gap-1 -mb-[1.5px]">
        {tabs.map((t) => {
          const on = t.id === active;
          return (
            <button
              key={t.id}
              role="tab"
              type="button"
              aria-selected={on}
              onClick={() => setActive(t.id)}
              className={
                "relative whitespace-nowrap border-[1.5px] border-ink px-3 py-1.5 text-[13.5px] " +
                (on ? "z-10 border-b-paper border-t-[5px] border-t-arkiv-orange bg-paper font-semibold" : "hatch hover:bg-paper")
              }
            >
              <span className={on ? "" : "bg-paper px-1"}>{t.label}</span>
            </button>
          );
        })}
      </div>
      <div role="tabpanel" className="sheet px-4 py-3">
        {tabs.find((t) => t.id === active)?.body}
      </div>
    </div>
  );
}

/** A handwritten margin note. */
export function Note({ children, className = "" }: { children: ReactNode; className?: string }) {
  return <p className={`font-hand text-[22px] leading-tight text-arkiv-blue ${className}`}>{children}</p>;
}

/** Section heading with a short ink rule under it. */
export function H2({ children, id }: { children: ReactNode; id?: string }) {
  return (
    <h2 id={id} className="text-[26px] sm:text-[30px] mt-2 mb-3">
      <span className="bg-paper">{children}</span>
    </h2>
  );
}

/** The Arkiv wordmark as Arkiv writes it: bracketed capitals, in Arkiv blue. */
export function ArkivMark({ className = "" }: { className?: string }) {
  return (
    <span className={`font-display font-bold tracking-[0.02em] whitespace-nowrap text-arkiv-blue ${className}`} aria-label="Arkiv">
      [&nbsp;ARKIV&nbsp;]
    </span>
  );
}
