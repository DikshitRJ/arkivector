import { useCallback, useRef, type KeyboardEvent, type PointerEvent } from "react";
import { Stage } from "../anim/film/Stage";
import { TALL, WIDE } from "../anim/film/layout";
import { CHAPTERS, LINES, TOTAL, chapterAt, lineAt } from "../anim/film/script";
import { useNarrow, useReducedMotion, useTimeline, type Timeline } from "../anim/time";
import { Page, REPO } from "../components/Layout";
import { Button, H2, Note } from "../components/ui";

const fmt = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

export function Architecture() {
  const tl = useTimeline(TOTAL * 1000);
  const narrow = useNarrow();
  const reduced = useReducedMotion();
  const t = tl.t / 1000;
  const L = narrow ? TALL : WIDE;
  const chapter = chapterAt(t);

  const jump = useCallback((s: number) => tl.seek(s * 1000), [tl]);

  const onKey = (e: KeyboardEvent) => {
    if (e.key === " " || e.key === "k") {
      e.preventDefault();
      tl.toggle();
    } else if (e.key === "ArrowRight") jump(Math.min(TOTAL, t + 5));
    else if (e.key === "ArrowLeft") jump(Math.max(0, t - 5));
  };

  return (
    <>
      <Page className="pt-10 sm:pt-14 pb-6">
        <h1 className="text-[38px] sm:text-[50px]">
          <span className="bg-paper">How it works</span>
        </h1>
        <p className="mt-3 max-w-[66ch] text-[16px] bg-paper">
          A write, a sync, a search, an expiry and a reorg, followed through the daemon. Ninety seconds, no sound. Subtitles run
          under the picture, and the full text is below if you would rather read.
        </p>
      </Page>

      <Page className="pb-12">
        <div
          className="sheet outline-none focus-visible:outline-2 focus-visible:outline-dashed"
          tabIndex={0}
          onKeyDown={onKey}
          aria-label="Architecture film. Space to play or pause, arrow keys to skip five seconds."
        >
          <div className="relative">
            <Stage t={t} L={L} />
            {!tl.playing && (t === 0 || tl.ended) && (
              <div className="absolute inset-0 grid place-items-center">
                <button
                  type="button"
                  onClick={tl.play}
                  className="whitespace-nowrap border-[1.5px] border-ink bg-ink text-paper px-6 py-3 text-[16px] font-medium shadow-[4px_4px_0_#fe7445] active:translate-x-1 active:translate-y-1 active:shadow-none"
                >
                  {tl.ended ? "watch again" : "▶ play the film"}
                </button>
              </div>
            )}
          </div>
          <Controls tl={tl} t={t} chapter={chapter} jump={jump} />
          <Subtitles t={t} narrow={narrow} />
        </div>
        {reduced && <p className="mt-3 text-[13px]">Reduced motion is on: the film waits for you to press play.</p>}
      </Page>

      <Page className="pb-16">
        <div className="grid md:grid-cols-[minmax(0,1fr)_16rem] gap-8 items-start">
          <div className="min-w-0">
            <H2>The same story, as text</H2>
            {CHAPTERS.map((c, ci) => (
              <section key={c.title} className="mb-6">
                <h3 className="text-[19px] mb-1">
                  <button type="button" className="text-left hover:underline underline-offset-4 bg-paper" onClick={() => jump(c.start)}>
                    {ci + 1}. {c.title} <span className="font-mono text-[12.5px] font-normal">({fmt(c.start)})</span>
                  </button>
                </h3>
                <p className="max-w-[72ch] bg-paper">
                  {LINES.filter((l) => l.t >= c.start && l.t < c.end)
                    .map((l) => l.text)
                    .join(" ")}
                </p>
              </section>
            ))}
          </div>
          <aside className="md:sticky md:top-6">
            <div className="sheet px-4 py-3">
              <p className="font-semibold mb-2">Where to look in the code</p>
              <ul className="space-y-1 text-[13px]">
                <li>
                  <code>daemon/src/server/</code> HTTP and REST
                </li>
                <li>
                  <code>daemon/src/write/</code> writer, nonce, batches
                </li>
                <li>
                  <code>daemon/src/arkiv/</code> ABI, RLP, signer
                </li>
                <li>
                  <code>daemon/src/sync/</code> syncer, TTL, reorgs
                </li>
                <li>
                  <code>daemon/src/filter/</code> filters, bitmaps
                </li>
                <li>
                  <code>daemon/src/pgvector/</code> copied pgvector code
                </li>
              </ul>
            </div>
            <Note className="mt-4 -rotate-2">the film runs on a 30-line timeline engine. no video file.</Note>
            <div className="mt-5">
              <Button href={REPO} variant="outline" external>
                Read the source
              </Button>
            </div>
          </aside>
        </div>
      </Page>
    </>
  );
}

function Controls({ tl, t, chapter, jump }: { tl: Timeline; t: number; chapter: number; jump: (s: number) => void }) {
  const barRef = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);

  const seekFromEvent = (e: PointerEvent) => {
    const el = barRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const k = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
    jump(k * TOTAL);
  };

  const prev = () => jump(t - CHAPTERS[chapter].start > 1.5 ? CHAPTERS[chapter].start : CHAPTERS[Math.max(0, chapter - 1)].start);
  const next = () => jump(chapter + 1 < CHAPTERS.length ? CHAPTERS[chapter + 1].start : TOTAL);

  const pct = (t / TOTAL) * 100;
  return (
    <div className="border-t-[1.5px] border-ink px-3 py-2">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
        <div className="flex items-center gap-2">
          <button
            type="button"
            onClick={tl.toggle}
            aria-label={tl.playing ? "Pause" : "Play"}
            className="w-20 whitespace-nowrap border-[1.5px] border-ink bg-ink text-paper py-1 text-[13px] shadow-[2px_2px_0_#000] active:translate-x-0.5 active:translate-y-0.5 active:shadow-none"
          >
            {tl.playing ? "pause" : "play"}
          </button>
          <button type="button" onClick={prev} aria-label="Previous chapter" className="whitespace-nowrap border-[1.5px] border-ink px-2 py-1 text-[13px] hover:bg-ink hover:text-paper">
            ‹ prev
          </button>
          <button type="button" onClick={next} aria-label="Next chapter" className="whitespace-nowrap border-[1.5px] border-ink px-2 py-1 text-[13px] hover:bg-ink hover:text-paper">
            next ›
          </button>
        </div>
        <p className="text-[13px] tabular-nums whitespace-nowrap">
          {fmt(t)} / {fmt(TOTAL)}
        </p>
        <p className="text-[13px] font-semibold truncate min-w-0 flex-1">
          {chapter + 1}. {CHAPTERS[chapter].title}
        </p>
      </div>

      {/* pencil-line progress with chapter ticks; click or drag to seek */}
      <div
        ref={barRef}
        className="relative mt-2 h-7 cursor-pointer touch-none"
        role="slider"
        aria-label="Seek"
        aria-valuemin={0}
        aria-valuemax={TOTAL}
        aria-valuenow={Math.round(t)}
        onPointerDown={(e) => {
          dragging.current = true;
          (e.target as Element).setPointerCapture?.(e.pointerId);
          seekFromEvent(e);
        }}
        onPointerMove={(e) => dragging.current && seekFromEvent(e)}
        onPointerUp={() => (dragging.current = false)}
      >
        <div className="absolute left-0 right-0 top-1/2 border-t border-dashed border-ink" />
        <div className="absolute left-0 top-1/2 -translate-y-1/2 h-[3px] bg-ink" style={{ width: `${pct}%` }} />
        {CHAPTERS.map((c, i) => (
          <button
            key={c.title}
            type="button"
            title={c.title}
            aria-label={`Chapter ${i + 1}: ${c.title}`}
            onPointerDown={(e) => e.stopPropagation()}
            onClick={() => jump(c.start)}
            className={`absolute top-1/2 -translate-y-1/2 -translate-x-1/2 h-4 w-[3px] ${i <= chapter ? "bg-ink" : "bg-paper border border-ink"}`}
            style={{ left: `${(c.start / TOTAL) * 100}%` }}
          />
        ))}
        <div
          className="absolute top-1/2 -translate-y-1/2 -translate-x-1/2 h-4 w-4 rounded-full bg-arkiv-orange border-[1.5px] border-ink"
          style={{ left: `${pct}%` }}
        />
      </div>
    </div>
  );
}

/** Subtitles scroll upward like a teleprompter; the current line is set in bold with an orange rule. */
function Subtitles({ t, narrow }: { t: number; narrow: boolean }) {
  const cur = lineAt(t);
  const LINE_H = narrow ? 82 : 54;
  return (
    <div className="relative border-t-[1.5px] border-ink overflow-hidden" style={{ height: LINE_H * 2 + 24 }} aria-live="polite">
      <div
        className="transition-transform duration-300 ease-out"
        style={{ transform: `translateY(${12 + LINE_H - cur * LINE_H}px)` }}
      >
        {LINES.map((l, i) => {
          const state = i < cur ? "past" : i === cur ? "now" : "next";
          return (
            <p
              key={l.t}
              style={{ height: LINE_H }}
              className={`flex items-center px-4 border-l-4 text-[14px] sm:text-[15px] leading-snug ${
                state === "now" ? "border-arkiv-orange font-semibold" : "border-transparent"
              } ${state === "next" ? "invisible" : ""}`}
              aria-hidden={state !== "now"}
            >
              {l.text}
            </p>
          );
        })}
      </div>
    </div>
  );
}
