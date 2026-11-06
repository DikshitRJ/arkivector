import { useCallback, useEffect, useRef, useState, type RefObject } from "react";

/* ---------- pure helpers: every animation is a function of time ---------- */

export const clamp = (x: number, a = 0, b = 1) => Math.min(b, Math.max(a, x));
/** Progress of p through the sub-range [a, b], clamped to 0..1. */
export const seg = (p: number, a: number, b: number) => clamp((p - a) / (b - a));
export const lerp = (a: number, b: number, k: number) => a + (b - a) * k;
/** easeInOutCubic */
export const ease = (x: number) => (x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2);
export const easeOut = (x: number) => 1 - Math.pow(1 - x, 3);

/** Orthographic projection of a 3D point, rotated by yaw (around y) then pitch (around x). */
export function project(
  v: readonly [number, number, number],
  yaw: number,
  pitch: number,
  scale: number,
  cx: number,
  cy: number,
): [number, number, number] {
  const [x, y, z] = v;
  const x1 = x * Math.cos(yaw) + z * Math.sin(yaw);
  const z1 = -x * Math.sin(yaw) + z * Math.cos(yaw);
  const y2 = y * Math.cos(pitch) - z1 * Math.sin(pitch);
  const z2 = y * Math.sin(pitch) + z1 * Math.cos(pitch);
  return [cx + x1 * scale, cy - y2 * scale, z2];
}

/* ---------- hooks ---------- */

/** `?freeze=4500` in the URL renders every animation at that time (for screenshots and review). */
export function frozenAt(): number | null {
  if (typeof window === "undefined") return null;
  const v = new URLSearchParams(window.location.search).get("freeze");
  return v !== null && !Number.isNaN(Number(v)) ? Number(v) : null;
}

export function useReducedMotion(): boolean {
  const [reduced, setReduced] = useState(
    () => typeof window !== "undefined" && window.matchMedia("(prefers-reduced-motion: reduce)").matches,
  );
  useEffect(() => {
    const mq = window.matchMedia("(prefers-reduced-motion: reduce)");
    const on = () => setReduced(mq.matches);
    mq.addEventListener("change", on);
    return () => mq.removeEventListener("change", on);
  }, []);
  return reduced;
}

export function useInView<T extends Element>(ref: RefObject<T | null>, margin = "80px"): boolean {
  const [inView, setInView] = useState(false);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const io = new IntersectionObserver(([e]) => setInView(e.isIntersecting), { rootMargin: margin });
    io.observe(el);
    return () => io.disconnect();
  }, [ref, margin]);
  return inView;
}

/**
 * Looping clock for decorative animations. Runs only while the element is on screen.
 * With reduced motion it returns `stillAt` (a representative frame) and never ticks.
 */
export function useLoop<T extends Element>(ref: RefObject<T | null>, periodMs: number, stillAt = periodMs * 0.95): number {
  const frozen = frozenAt();
  const reduced = useReducedMotion() || frozen !== null;
  if (frozen !== null) stillAt = frozen % periodMs;
  const inView = useInView(ref);
  const [t, setT] = useState(reduced ? stillAt : 0);
  const base = useRef<number | null>(null);
  const acc = useRef(0);
  useEffect(() => {
    if (reduced) {
      setT(stillAt);
      return;
    }
    if (!inView) return;
    let raf = 0;
    base.current = null;
    const tick = (now: number) => {
      if (base.current === null) base.current = now - acc.current;
      acc.current = now - base.current;
      setT(acc.current % periodMs);
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [inView, reduced, periodMs, stillAt]);
  return t;
}

export type Timeline = {
  t: number;
  playing: boolean;
  ended: boolean;
  play: () => void;
  pause: () => void;
  toggle: () => void;
  seek: (ms: number) => void;
};

/** Seekable, pausable clock for the architecture film. Stops at `total`. */
export function useTimeline(total: number): Timeline {
  const [t, setT] = useState(() => frozenAt() ?? 0);
  const [playing, setPlaying] = useState(false);
  const tRef = useRef(0);
  tRef.current = t;

  useEffect(() => {
    if (!playing) return;
    let raf = 0;
    let last: number | null = null;
    const tick = (now: number) => {
      if (last !== null) {
        const next = Math.min(total, tRef.current + (now - last));
        tRef.current = next;
        setT(next);
        if (next >= total) {
          setPlaying(false);
          return;
        }
      }
      last = now;
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [playing, total]);

  const play = useCallback(() => {
    if (tRef.current >= total) setT(0);
    setPlaying(true);
  }, [total]);
  const pause = useCallback(() => setPlaying(false), []);
  const toggle = useCallback(() => (playing ? pause() : play()), [playing, play, pause]);
  const seek = useCallback((ms: number) => setT(clamp(ms, 0, total)), [total]);

  return { t, playing, ended: t >= total, play, pause, toggle, seek };
}

/** True below the given width (matches Tailwind's md breakpoint by default). */
export function useNarrow(max = 767): boolean {
  const q = `(max-width: ${max}px)`;
  const [narrow, setNarrow] = useState(() => typeof window !== "undefined" && window.matchMedia(q).matches);
  useEffect(() => {
    const mq = window.matchMedia(q);
    const on = () => setNarrow(mq.matches);
    mq.addEventListener("change", on);
    return () => mq.removeEventListener("change", on);
  }, [q]);
  return narrow;
}
