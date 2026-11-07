import { useCallback, useEffect, useRef, useState, type KeyboardEvent, type PointerEvent } from "react";
import { ArchDiagram } from "../anim/ArchDiagram";
import { Film } from "../anim/film/Stage";
import { TALL, WIDE } from "../anim/film/layout";
import { CHAPTERS, LINES, TOTAL, chapterAt, lineAt } from "../anim/film/script";
import { useNarrow, useReducedMotion, useTimeline, type Timeline } from "../anim/time";
import { speak, speechSupported, stopSpeaking } from "../audio/narrator";
import { useSound } from "../audio/sound";
import { Page, REPO } from "../components/Layout";
import { Button, H2, Note } from "../components/ui";

const fmt = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;
const VOICE_KEY = "arkivector-voice";

function readVoicePref(): boolean {
  try {
    return localStorage.getItem(VOICE_KEY) !== "off";
  } catch {
    return true;
  }
}

export function Architecture() {
  const narrow = useNarrow();
  const reduced = useReducedMotion();
  const { duck } = useSound();
  const [voice, setVoice] = useState(() => speechSupported() && readVoicePref());
  const speaking = useRef(false);
  const curRef = useRef(0);

  // While the narrator is reading a line, the clock may not pass the start of the next line.
  const limit = useCallback(() => {
    if (!speaking.current) return null;
    const next = LINES[curRef.current + 1];
    return next ? next.t * 1000 - 30 : null;
  }, []);
  const tl = useTimeline(TOTAL * 1000, limit);
  const t = tl.t / 1000;
  const L = narrow ? TALL : WIDE;
  const chapter = chapterAt(t);
  const cur = lineAt(t);
  curRef.current = cur;

  // narrate the current line
  useEffect(() => {
    if (!tl.playing || !voice) return;
    speaking.current = true;
    duck(true);
    const cancel = speak(LINES[cur].say ?? LINES[cur].text, () => {
      speaking.current = false;
      duck(false);
    });
    return () => {
      cancel();
      speaking.current = false;
      duck(false);
    };
  }, [cur, tl.playing, voice, duck]);

  useEffect(() => () => stopSpeaking(), []);

  const toggleVoice = () => {
    setVoice((v) => {
      const next = !v;
      try {
        localStorage.setItem(VOICE_KEY, next ? "on" : "off");
      } catch {
        /* ignore */
      }
      if (!next) stopSpeaking();
      return next;
    });
  };

  const filmRef = useRef<HTMLDivElement>(null);
  const jump = useCallback((s: number) => tl.seek(s * 1000), [tl]);
  const jumpAndShow = (s: number) => {
    jump(s);
    filmRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
  };

  const onKey = (e: KeyboardEvent) => {
    if (e.key === " " || e.key === "k") {
      e.preventDefault();
      tl.toggle();
    } else if (e.key === "ArrowRight") jump(Math.min(TOTAL, t + 5));
    else if (e.key === "ArrowLeft") jump(Math.max(0, t - 5));
    else if (e.key === "m") toggleVoice();
  };

  return (
    <>
      <Page className="pt-10 sm:pt-14 pb-6">
        <h1 className="text-[38px] sm:text-[50px]">
          <span className="bg-paper">How it works</span>
        </h1>
        <p className="mt-3 max-w-[64ch] text-[16px] bg-paper">
          A write, a sync and a search followed through the daemon, then the math that ranks the results.
        </p>
      </Page>

      <Page className="pb-12">
        <H2>The big picture</H2>
        <ArchDiagram />
        <p className="mt-3 max-w-[72ch] text-[13.5px] bg-paper">
          Your code talks to <code className="chip">arkivectord</code> through an SDK. The daemon writes to Arkiv with signed
          transactions and reads entities back into its local index. Search never leaves the daemon.
        </p>
      </Page>

      <Page className="pb-12">
        <div ref={filmRef} className="scroll-mt-4">
          <H2>The film</H2>
        </div>
        <div
          className="sheet outline-none focus-visible:outline-2 focus-visible:outline-dashed"
          tabIndex={0}
          onKeyDown={onKey}
          aria-label="Architecture film. Space to play or pause, arrow keys to skip five seconds, m to mute the narrator."
        >
          <div className="relative">
            <Film t={t} L={L} />
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
          <Controls tl={tl} t={t} chapter={chapter} jump={jump} voice={voice} toggleVoice={toggleVoice} />
          <Subtitles cur={cur} narrow={narrow} />
        </div>
        {reduced && <p className="mt-3 text-[13px]">Reduced motion is on: the film waits for you to press play.</p>}
        {!speechSupported() && <p className="mt-3 text-[13px]">This browser has no speech synthesis, so the film plays with subtitles only.</p>}
      </Page>

      <Page className="pb-16">
        <H2>The same story, short</H2>
        <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-4">
          {CHAPTERS.map((c, i) => (
            <button
              key={c.title}
              type="button"
              onClick={() => jumpAndShow(c.start)}
              className="sheet text-left hover:shadow-[3px_3px_0_#fe7445] transition-shadow"
            >
              <div className="pointer-events-none border-b border-dashed border-ink">
                <Film t={c.still} L={WIDE} />
              </div>
              <div className="px-3 py-2">
                <p className="font-semibold text-[14px]">
                  {i + 1}. {c.title} <span className="font-normal text-[12px]">{fmt(c.start)}</span>
                </p>
                <p className="text-[13px] leading-snug">{c.summary}</p>
              </div>
            </button>
          ))}
        </div>
        <div className="mt-8 flex flex-wrap items-center gap-4">
          <Button href={REPO} variant="outline" external>
            Read the source
          </Button>
          <Note className="-rotate-1">no video file: every frame is drawn from the time value.</Note>
        </div>
      </Page>
    </>
  );
}

function Controls({
  tl,
  t,
  chapter,
  jump,
  voice,
  toggleVoice,
}: {
  tl: Timeline;
  t: number;
  chapter: number;
  jump: (s: number) => void;
  voice: boolean;
  toggleVoice: () => void;
}) {
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
  const small = "whitespace-nowrap border-[1.5px] border-ink px-2 py-1 text-[13px] hover:bg-ink hover:text-paper";

  const pct = (t / TOTAL) * 100;
  return (
    <div className="border-t-[1.5px] border-ink px-3 py-2">
      <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
        <div className="flex flex-wrap items-center gap-2">
          <button
            type="button"
            onClick={tl.toggle}
            aria-label={tl.playing ? "Pause" : "Play"}
            className="w-20 whitespace-nowrap border-[1.5px] border-ink bg-ink text-paper py-1 text-[13px] shadow-[2px_2px_0_#000] active:translate-x-0.5 active:translate-y-0.5 active:shadow-none"
          >
            {tl.playing ? "pause" : "play"}
          </button>
          <button type="button" onClick={prev} aria-label="Previous chapter" className={small}>
            ‹ prev
          </button>
          <button type="button" onClick={next} aria-label="Next chapter" className={small}>
            next ›
          </button>
          {speechSupported() && (
            <button
              type="button"
              onClick={toggleVoice}
              aria-pressed={voice}
              title="Narrator (m)"
              className={`${small} ${voice ? "border-arkiv-orange shadow-[2px_2px_0_#fe7445]" : ""}`}
            >
              {voice ? "voice on" : "voice off"}
            </button>
          )}
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
function Subtitles({ cur, narrow }: { cur: number; narrow: boolean }) {
  const LINE_H = narrow ? 82 : 54;
  return (
    <div className="relative border-t-[1.5px] border-ink overflow-hidden" style={{ height: LINE_H * 2 + 24 }} aria-live="polite">
      <div className="transition-transform duration-300 ease-out" style={{ transform: `translateY(${12 + LINE_H - cur * LINE_H}px)` }}>
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
