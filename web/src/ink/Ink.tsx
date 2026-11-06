import type { SVGProps } from "react";
import { arrowHead, line, rect, scribble, type Pt } from "./paths";

/**
 * A path that draws itself as `p` goes from 0 to 1. pathLength=1 lets the dash trick work without
 * measuring the path.
 */
export function Stroke({
  d,
  p = 1,
  w = 1.6,
  dash,
  ...rest
}: { d: string; p?: number; w?: number; dash?: string } & SVGProps<SVGPathElement>) {
  if (p <= 0) return null;
  const drawing = p < 1;
  return (
    <path
      d={d}
      fill="none"
      stroke="#000"
      strokeWidth={w}
      strokeLinecap="round"
      strokeLinejoin="round"
      pathLength={drawing ? 1 : undefined}
      strokeDasharray={drawing ? "1" : dash}
      strokeDashoffset={drawing ? 1 - p : undefined}
      {...rest}
    />
  );
}

/** Hand-drawn box; `fill` is white by default so it masks the dot grid / lines behind it. */
export function Box({
  x,
  y,
  w,
  h,
  seed = 1,
  p = 1,
  sw = 1.6,
  fill = "#fff",
  hatch = false,
}: {
  x: number;
  y: number;
  w: number;
  h: number;
  seed?: number;
  p?: number;
  sw?: number;
  fill?: string;
  hatch?: boolean;
}) {
  if (p <= 0) return null;
  return (
    <g>
      {p >= 1 && <rect x={x} y={y} width={w} height={h} fill={hatch ? "url(#hatch)" : fill} />}
      <Stroke d={rect(x, y, w, h, seed)} p={p} w={sw} />
    </g>
  );
}

/** Hand-drawn arrow from a to b. */
export function Arrow({ a, b, seed = 1, p = 1, w = 1.6, dash }: { a: Pt; b: Pt; seed?: number; p?: number; w?: number; dash?: string }) {
  if (p <= 0) return null;
  const tip: Pt = [a[0] + (b[0] - a[0]) * Math.min(1, p), a[1] + (b[1] - a[1]) * Math.min(1, p)];
  return (
    <g>
      <Stroke d={line(a, tip, seed)} w={w} dash={dash} />
      <Stroke d={arrowHead(a, tip, 8 + w)} w={w} />
    </g>
  );
}

/** Shared SVG defs (hatch pattern). Put once inside any svg that uses <Box hatch />. */
export function InkDefs() {
  return (
    <defs>
      <pattern id="hatch" width="6" height="6" patternUnits="userSpaceOnUse">
        <rect width="6" height="6" fill="#fff" />
        <path d="M-1 7 L7 -1" stroke="#000" strokeWidth="1" />
      </pattern>
    </defs>
  );
}

/** Scribbled underline used for the active navigation item. */
export function Underline({ width, seed = 3 }: { width: number; seed?: number }) {
  return (
    <svg
      aria-hidden
      className="pointer-events-none absolute left-0 -bottom-1.5"
      width={width}
      height={8}
      viewBox={`0 0 ${width} 8`}
    >
      <path d={scribble(1, 4, width - 2, seed)} fill="none" stroke="#000" strokeWidth={1.8} strokeLinecap="round" />
    </svg>
  );
}
