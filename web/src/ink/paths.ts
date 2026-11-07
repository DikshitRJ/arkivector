/**
 * Hand-drawn path generators. Every function is seeded, so a line wobbles the same way on every
 * frame (no jitter) and on every render.
 */

export type Pt = readonly [number, number];

/** mulberry32: tiny deterministic PRNG returning values in [0, 1). */
export function rng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const f = (n: number) => Math.round(n * 10) / 10;

/** A slightly bowed line from a to b, like a pen stroke. amp is the maximum bow in units. */
export function line(a: Pt, b: Pt, seed = 1, amp = 1.6): string {
  const r = rng(seed);
  const dx = b[0] - a[0];
  const dy = b[1] - a[1];
  const len = Math.hypot(dx, dy) || 1;
  const nx = -dy / len;
  const ny = dx / len;
  const bow = (r() - 0.5) * 2 * Math.min(amp, len * 0.04);
  const k1 = 0.3 + r() * 0.1;
  const k2 = 0.65 + r() * 0.1;
  const c1: Pt = [a[0] + dx * k1 + nx * bow, a[1] + dy * k1 + ny * bow];
  const c2: Pt = [a[0] + dx * k2 + nx * bow * 0.6, a[1] + dy * k2 + ny * bow * 0.6];
  // start a hair off the corner, as a pen would
  const s: Pt = [a[0] + (r() - 0.5) * 0.8, a[1] + (r() - 0.5) * 0.8];
  return `M${f(s[0])} ${f(s[1])} C${f(c1[0])} ${f(c1[1])} ${f(c2[0])} ${f(c2[1])} ${f(b[0])} ${f(b[1])}`;
}

/** A rectangle drawn as four separate pen strokes whose corners overshoot a little. */
export function rect(x: number, y: number, w: number, h: number, seed = 1, amp = 1.4): string {
  const r = rng(seed * 7 + 3);
  const o = () => (r() - 0.3) * 2.2;
  return [
    line([x - o(), y], [x + w + o(), y], seed + 1, amp),
    line([x + w, y - o()], [x + w, y + h + o()], seed + 2, amp),
    line([x + w + o(), y + h], [x - o(), y + h], seed + 3, amp),
    line([x, y + h + o()], [x, y - o()], seed + 4, amp),
  ].join(" ");
}

/** Arrow head at point `tip`, pointing along the direction from `from` to `tip`. */
export function arrowHead(from: Pt, tip: Pt, size = 9): string {
  const ang = Math.atan2(tip[1] - from[1], tip[0] - from[0]);
  const a1 = ang + Math.PI - 0.45;
  const a2 = ang + Math.PI + 0.45;
  const p1: Pt = [tip[0] + Math.cos(a1) * size, tip[1] + Math.sin(a1) * size];
  const p2: Pt = [tip[0] + Math.cos(a2) * size, tip[1] + Math.sin(a2) * size];
  return `M${f(p1[0])} ${f(p1[1])} L${f(tip[0])} ${f(tip[1])} L${f(p2[0])} ${f(p2[1])}`;
}

/** A loose hand-drawn loop around a box, for "this one" circling. */
export function loop(cx: number, cy: number, rx: number, ry: number, seed = 1): string {
  const r = rng(seed);
  const pts: Pt[] = [];
  const n = 18;
  const start = r() * Math.PI * 2;
  for (let i = 0; i <= n + 2; i++) {
    const a = start + (i / n) * Math.PI * 2;
    const k = 1 + (r() - 0.5) * 0.12 + (i > n ? 0.08 : 0);
    pts.push([cx + Math.cos(a) * rx * k, cy + Math.sin(a) * ry * k]);
  }
  return smooth(pts);
}

/** Catmull-Rom through points, as a cubic bezier path. */
export function smooth(pts: Pt[]): string {
  if (pts.length < 2) return "";
  let d = `M${f(pts[0][0])} ${f(pts[0][1])}`;
  for (let i = 0; i < pts.length - 1; i++) {
    const p0 = pts[i - 1] ?? pts[i];
    const p1 = pts[i];
    const p2 = pts[i + 1];
    const p3 = pts[i + 2] ?? p2;
    const c1: Pt = [p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6];
    const c2: Pt = [p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6];
    d += ` C${f(c1[0])} ${f(c1[1])} ${f(c2[0])} ${f(c2[1])} ${f(p2[0])} ${f(p2[1])}`;
  }
  return d;
}

/** A quick zig-zag underline scribble from x to x+w at height y. */
export function scribble(x: number, y: number, w: number, seed = 1): string {
  const r = rng(seed);
  const pts: Pt[] = [];
  const steps = Math.max(3, Math.round(w / 14));
  for (let i = 0; i <= steps; i++) {
    pts.push([x + (w * i) / steps, y + (i % 2 ? -1.4 : 1.4) + (r() - 0.5)]);
  }
  return smooth(pts);
}

/** Arc of a circle (for angle markers), angles in radians, y pointing down. */
export function arc(cx: number, cy: number, rad: number, a0: number, a1: number): string {
  const p0: Pt = [cx + Math.cos(a0) * rad, cy + Math.sin(a0) * rad];
  const p1: Pt = [cx + Math.cos(a1) * rad, cy + Math.sin(a1) * rad];
  const large = Math.abs(a1 - a0) > Math.PI ? 1 : 0;
  const sweep = a1 > a0 ? 1 : 0;
  return `M${f(p0[0])} ${f(p0[1])} A${f(rad)} ${f(rad)} 0 ${large} ${sweep} ${f(p1[0])} ${f(p1[1])}`;
}
