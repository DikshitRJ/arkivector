import type { ReactNode } from "react";
import { BLUE, ORANGE } from "../../ink/colors";
import { Arrow, Box, InkDefs, Stroke } from "../../ink/Ink";
import { arc, line, loop } from "../../ink/paths";
import { seg } from "../time";
import type { Layout } from "./layout";

/*
 * Chapters 6 to 8 of the film: the arithmetic of the search, halfvec encoding, and exact scan
 * versus HNSW. All numbers are computed from the same vectors the earlier chapters use.
 */

const MONO = { fontFamily: "var(--font-mono)" } as const;
const HAND = { fontFamily: "var(--font-hand)" } as const;
const DISP = { fontFamily: "var(--font-display)" } as const;

function T({ x, y, s, children, w = 400, a = "start", fill = "#000", hand = false, disp = false }: {
  x: number; y: number; s: number; children: ReactNode; w?: number; a?: "start" | "middle" | "end"; fill?: string; hand?: boolean; disp?: boolean;
}) {
  return (
    <text x={x} y={y} fontSize={s} fontWeight={w} textAnchor={a} fill={fill} style={hand ? HAND : disp ? DISP : MONO}>
      {children}
    </text>
  );
}

/** Text that writes itself left to right while p goes 0 to 1. */
function Typed({ p, text, ...rest }: { p: number; text: string } & Omit<Parameters<typeof T>[0], "children">) {
  if (p <= 0) return null;
  return <T {...rest}>{text.slice(0, Math.max(1, Math.round(text.length * p)))}</T>;
}

export function MathScenes({ t, L }: { t: number; L: Layout }) {
  return (
    <svg viewBox={`0 0 ${L.vw} ${L.vh}`} className="block w-full h-auto select-none" role="img" aria-label="Worked examples of the vector math used by Arkivector">
      <InkDefs />
      {t < 74 ? <Arithmetic s={t - 56} L={L} /> : t < 86 ? <HalfFloat s={t - 74} L={L} /> : <ScanVsGraph s={t - 86} L={L} />}
    </svg>
  );
}

/* ---------------- 6. the arithmetic ---------------- */

const WORK: { at: number; text: string; hot?: boolean; gap?: boolean }[] = [
  { at: 0.4, text: "q   = [1, 0.05, 0, 0]" },
  { at: 3.4, text: "d(u,q) = 1 − (u·q) / (‖u‖ ‖q‖)", gap: true },
  { at: 6.8, text: "cat = [1, 0, 0, 0]", gap: true },
  { at: 7.8, text: "u·q = 1·1 + 0·0.05 = 1" },
  { at: 8.8, text: "‖q‖ = √(1 + 0.0025) = 1.00125" },
  { at: 9.8, text: "d   = 1 − 1/1.00125 = 0.00125", hot: true },
  { at: 11.2, text: "dog = [0.9, 0.1, 0, 0]", gap: true },
  { at: 12.2, text: "u·q = 0.9 + 0.005 = 0.905" },
  { at: 13.0, text: "‖u‖ = √0.82 = 0.9055" },
  { at: 13.8, text: "d   = 1 − 0.905/(0.9055·1.00125)" },
  { at: 14.4, text: "    = 0.00184", hot: true },
  { at: 15.0, text: "L2: cat 0.050 · dog 0.112, same order", gap: true },
];

function Arithmetic({ s, L }: { s: number; L: Layout }) {
  const wide = L.vertical;
  const fs = L.fs;
  // sketch panel
  const sk = wide ? { x: 24, y: 24, w: 430, h: 492 } : { x: 18, y: 18, w: 504, h: 300 };
  const wk = wide ? { x: 478, y: 24, w: 458, h: 492 } : { x: 18, y: 334, w: 504, h: 512 };
  const ox = sk.x + 50;
  const oy = sk.y + sk.h - 60;
  const R = wide ? 330 : 400;
  const X = 6; // angle exaggeration so 3 and 6 degree gaps are visible
  const deg = (d: number) => (-(d * X) * Math.PI) / 180;
  const vec = (d: number, len: number): [number, number] => [ox + Math.cos(deg(d)) * len, oy + Math.sin(deg(d)) * len];
  const pSk = seg(s, 0, 1);
  const qA = (Math.atan(0.05) * 180) / Math.PI; // 2.86 degrees
  const dogA = (Math.atan(0.1 / 0.9) * 180) / Math.PI; // 6.34 degrees
  const step = 30 * fs;
  let y = wk.y + 40 * fs;

  return (
    <g>
      <Box x={sk.x} y={sk.y} w={sk.w} h={sk.h} seed={1201} p={pSk} />
      {pSk >= 1 && (
        <>
          <Stroke d={line([ox - 20, oy], [sk.x + sk.w - 20, oy], 1202)} w={1} dash="4 4" />
          <Stroke d={line([ox, oy + 20], [ox, sk.y + 20], 1203)} w={1} dash="4 4" />
          <Arrow a={[ox, oy]} b={vec(0, R)} seed={1204} w={1.8} p={seg(s, 6.8, 7.6)} />
          <Arrow a={[ox, oy]} b={vec(dogA, R * 0.905)} seed={1205} w={1.8} p={seg(s, 11.2, 12)} />
          <Arrow a={[ox, oy]} b={vec(qA, R * 1.0)} seed={1206} w={2.8} color={ORANGE} p={seg(s, 0.6, 1.6)} />
          {s >= 1.6 && <T x={vec(qA, R)[0] + 6} y={vec(qA, R)[1] - 4} s={14 * fs} w={700} fill={ORANGE}>q</T>}
          {s >= 7.6 && <T x={vec(0, R)[0] - 4} y={vec(0, R)[1] + 22 * fs} s={14 * fs} w={700} a="end">cat</T>}
          {s >= 12 && <T x={vec(dogA, R * 0.905)[0] + 6} y={vec(dogA, R * 0.905)[1] - 6} s={14 * fs} w={700}>dog</T>}
          {s >= 10 && <Stroke d={arc(ox, oy, R * 0.5, deg(qA), deg(0))} w={1.6} stroke={ORANGE} />}
          {s >= 14.4 && <Stroke d={arc(ox, oy, R * 0.38, deg(dogA), deg(qA))} w={1.6} />}
          <T x={sk.x + 14} y={sk.y + 26 * fs} s={13 * fs} w={700}>
            the vectors, first two dimensions
          </T>
          <T x={sk.x + sk.w - 14} y={sk.y + sk.h - 14} s={17 * fs} a="end" fill={BLUE} hand>
            angles drawn 6× wider than they are
          </T>
        </>
      )}

      <Box x={wk.x} y={wk.y} w={wk.w} h={wk.h} seed={1210} p={seg(s, 0.2, 1.2)} />
      {WORK.map((w, i) => {
        if (w.gap) y += 10 * fs;
        y += i ? step * 0.82 : 0;
        const p = seg(s, w.at, w.at + 0.9);
        return (
          <g key={i}>
            {w.hot && p >= 1 && <rect x={wk.x + 12} y={y - 18 * fs} width={wk.w - 24} height={25 * fs} fill={ORANGE} />}
            <Typed p={p} text={w.text} x={wk.x + 18} y={y} s={14 * fs} w={w.hot ? 700 : 500} />
          </g>
        );
      })}
      {s >= 16.2 && (
        <T x={wk.x + wk.w - 16} y={wk.y + wk.h - 16} s={19 * fs} a="end" fill={BLUE} hand>
          the numbers the search returned
        </T>
      )}
    </g>
  );
}

/* ---------------- 7. halfvec ---------------- */

const BITS = "0010111001100110"; // 0x2E66, binary16 for 0.1

function HalfFloat({ s, L }: { s: number; L: Layout }) {
  const wide = L.vertical;
  const fs = L.fs;
  const cw = wide ? 46 : 29;
  const gapX = (i: number) => (i >= 1 ? 8 : 0) + (i >= 6 ? 8 : 0);
  const total = 16 * cw + 16;
  const x0 = (L.vw - total) / 2;
  const by = wide ? 170 : 230;
  const bh = wide ? 56 : 46;
  const groups = [
    { from: 0, to: 1, label: "sign 0", c: "#000" },
    { from: 1, to: 6, label: "exponent 01011 = 11", c: "#000" },
    { from: 6, to: 16, label: "mantissa 1001100110 = 614", c: ORANGE },
  ];
  const mx = (i: number) => x0 + i * cw + gapX(i);
  const lines: { at: number; text: string; hot?: boolean }[] = [
    { at: 4.4, text: "value = (−1)^s · 2^(e−15) · (1 + m/1024)" },
    { at: 5.6, text: "      = 2^(11−15) · (1 + 614/1024)" },
    { at: 6.6, text: "      = 0.0625 · 1.599609375 = 0.0999755859375", hot: true },
    { at: 8.4, text: "float32 0.1 → 0.100000001490116" },
    { at: 9.2, text: "halfvec 0.1 → 0.0999755859375   (0.024 % off, half the bytes)" },
  ];
  const ly0 = wide ? 330 : 400;
  return (
    <g>
      <T x={L.vw / 2} y={wide ? 70 : 90} s={22 * fs} w={700} a="middle" disp>
        0.1 as halfvec (IEEE 754 binary16)
      </T>
      {BITS.split("").map((b, i) => {
        const p = seg(s, 0.6 + i * 0.08, 1 + i * 0.08);
        const man = i >= 6;
        return (
          <g key={i}>
            <rect x={mx(i)} y={by} width={cw - 4} height={bh} fill={man && b === "1" && s >= 3.2 ? ORANGE : "#fff"} stroke="#000" strokeWidth={p > 0 ? 1.5 : 0} />
            {p >= 1 && (
              <T x={mx(i) + (cw - 4) / 2} y={by + bh / 2 + 7 * fs} s={20 * fs} w={700} a="middle">
                {b}
              </T>
            )}
          </g>
        );
      })}
      {s >= 2.2 &&
        groups.map((g, i) => {
          const a = mx(g.from);
          const b = mx(g.to - 1) + cw - 4;
          return (
            <g key={g.label}>
              <Stroke d={`M${a} ${by + bh + 10} v6 H${b} v-6`} w={1.4} stroke={g.c} p={seg(s, 2.2 + i * 0.3, 2.8 + i * 0.3)} />
              <T x={(a + b) / 2} y={by + bh + (wide ? 38 : 40 + (i % 2) * 22)} s={(wide ? 17 : 16) * fs} a="middle" fill={BLUE} hand>
                {g.label}
              </T>
            </g>
          );
        })}
      {lines.map((l, i) => {
        const y = ly0 + i * 34 * fs + (i >= 3 ? 14 * fs : 0);
        const p = seg(s, l.at, l.at + 1);
        return (
          <g key={i}>
            {l.hot && p >= 1 && <rect x={x0 - 8} y={y - 19 * fs} width={Math.min(total + 16, L.vw - 2 * x0 + 16)} height={26 * fs} fill={ORANGE} />}
            <Typed p={p} text={l.text} x={x0} y={y} s={(wide ? 15 : 12.2) * fs} w={l.hot ? 700 : 500} />
          </g>
        );
      })}
      {s >= 7.6 && (
        <T x={L.vw - 24} y={L.vh - 22} s={19 * fs} a="end" fill={BLUE} hand>
          rounded half to even, with pgvector's own conversion code
        </T>
      )}
    </g>
  );
}

/* ---------------- 8. exact scan vs HNSW ---------------- */

const CANDS = [
  { id: "fish", d: 0.0311 },
  { id: "owl", d: 0.41 },
  { id: "cat", d: 0.0012 },
  { id: "car", d: 0.99 },
  { id: "dog", d: 0.0018 },
  { id: "ant", d: 0.52 },
];

function ScanVsGraph({ s, L }: { s: number; L: Layout }) {
  const wide = L.vertical;
  const fs = L.fs;
  const A = wide ? { x: 24, y: 24, w: 440, h: 492 } : { x: 18, y: 18, w: 504, h: 400 };
  const B = wide ? { x: 488, y: 24, w: 448, h: 492 } : { x: 18, y: 434, w: 504, h: 412 };
  const scanned = Math.min(CANDS.length, Math.floor(seg(s, 0.8, 6.2) * CANDS.length + 0.0001));
  const heap = CANDS.slice(0, scanned)
    .slice()
    .sort((a, b) => a.d - b.d)
    .slice(0, 2);
  const rowH = 34 * fs;

  // HNSW sketch inside B
  const layerY = [B.y + 110 * fs, B.y + 190 * fs, B.y + 270 * fs];
  const span = (k: number) => B.x + 30 + (k * (B.w - 60)) / 7;
  const nodes = [[1, 5], [1, 3, 5, 6], [0, 1, 2, 3, 4, 5, 6, 7]];
  const path: [number, number][] = [
    [span(1), layerY[0]],
    [span(5), layerY[0]],
    [span(5), layerY[1]],
    [span(6), layerY[1]],
    [span(6), layerY[2]],
    [span(7), layerY[2]],
  ];
  const pg = seg(s, 7.6, 10.6);
  const nPath = Math.floor(pg * (path.length - 1) + 0.0001);

  return (
    <g>
      {/* exact scan */}
      <Box x={A.x} y={A.y} w={A.w} h={A.h} seed={1301} p={seg(s, 0, 0.6)} />
      <T x={A.x + 16} y={A.y + 30 * fs} s={16 * fs} w={700} disp>
        exact scan (small sets)
      </T>
      {CANDS.map((c, i) => {
        const y = A.y + 66 * fs + i * rowH;
        const done = i < scanned;
        const cur = i === scanned && s < 6.2;
        const kept = heap.some((h) => h.id === c.id);
        return (
          <g key={c.id}>
            {cur && <Stroke d={`M${A.x + 10} ${y - 5 * fs} l10 0`} w={3} stroke={ORANGE} />}
            <T x={A.x + 30} y={y} s={14 * fs} w={kept ? 700 : 400}>
              {c.id}
            </T>
            {done && (
              <T x={A.x + 30 + 140 * (wide ? 1 : 1.25)} y={y} s={14 * fs} a="end" w={kept ? 700 : 400}>
                {c.d.toFixed(4)}
              </T>
            )}
            {done && kept && <Stroke d={loop(A.x + 30 + 70 * (wide ? 1 : 1.25), y - 5 * fs, 92 * (wide ? 1 : 1.2), 13 * fs, 1310 + i)} w={1.8} stroke={ORANGE} />}
          </g>
        );
      })}
      {/* heap */}
      {(() => {
        const hx = A.x + A.w - (wide ? 150 : 170);
        const hy = A.y + 66 * fs - 20 * fs;
        return (
          <g>
            <Box x={hx} y={hy} w={wide ? 130 : 150} h={92 * fs} seed={1320} color={ORANGE} sw={2} />
            <T x={hx + 10} y={hy + 22 * fs} s={12 * fs} w={700}>
              heap, k = 2
            </T>
            {heap.map((h, i) => (
              <T key={h.id} x={hx + 10} y={hy + (48 + i * 24) * fs} s={13 * fs}>
                {`${h.id} ${h.d.toFixed(4)}`}
              </T>
            ))}
          </g>
        );
      })()}
      {s >= 3.4 && (
        <T x={A.x + 16} y={A.y + A.h - 24} s={14 * fs} w={600}>
          cost: n distances, O(n log k)
        </T>
      )}

      {/* HNSW */}
      <Box x={B.x} y={B.y} w={B.w} h={B.h} seed={1330} p={seg(s, 7, 7.6)} color={BLUE} />
      {s >= 7.6 && (
        <>
          <T x={B.x + 16} y={B.y + 30 * fs} s={16 * fs} w={700} disp>
            HNSW (large sets)
          </T>
          {layerY.map((ly, li) => (
            <g key={li}>
              <Stroke d={line([B.x + 16, ly], [B.x + B.w - 16, ly], 1340 + li)} w={0.8} dash="1 5" />
              {nodes[li].slice(0, -1).map((k, i) => (
                <Stroke key={i} d={line([span(k), ly], [span(nodes[li][i + 1]), ly], 1350 + li * 10 + i)} w={1.1} />
              ))}
              {nodes[li].map((k) => (
                <circle key={k} cx={span(k)} cy={ly} r={6 * fs} fill="#fff" stroke="#000" strokeWidth={1.5} />
              ))}
              <T x={B.x + B.w - 18} y={ly - 12} s={11.5 * fs} a="end">{`layer ${2 - li}`}</T>
            </g>
          ))}
          {path.slice(1, nPath + 1).map((pt, i) => (
            <Arrow key={i} a={path[i]} b={pt} seed={1370 + i} w={2.2} color={ORANGE} />
          ))}
          <circle cx={path[nPath][0]} cy={path[nPath][1]} r={7 * fs} fill={ORANGE} stroke="#000" strokeWidth={1.5} />
          {s >= 10.8 && (
            <>
              <T x={B.x + 16} y={B.y + B.h - 62 * fs} s={14 * fs} w={600}>
                ℓ = ⌊−ln U · m_L⌋,  m_L = 1 / ln M
              </T>
              <T x={B.x + 16} y={B.y + B.h - 38 * fs} s={13 * fs}>
                M = 16 links, ef_search = 40
              </T>
            </>
          )}
          {s >= 12 && (
            <T x={B.x + B.w - 16} y={B.y + B.h - 12} s={17 * fs} a="end" fill={BLUE} hand>
              visits 6 of 14 nodes instead of all of them
            </T>
          )}
        </>
      )}
    </g>
  );
}
