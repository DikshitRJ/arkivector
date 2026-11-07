import { useRef } from "react";
import { BLUE, ORANGE } from "../ink/colors";
import { Arrow, Stroke } from "../ink/Ink";
import { arc, loop } from "../ink/paths";
import { useLoop } from "./time";

/*
 * Cosine search in two dimensions. The query arrow swings back and forth; for every frame the
 * distance d = 1 - cos(theta) to each stored vector is computed and the table re-sorts. k = 2.
 */

const PERIOD = 16000;
const ITEMS = [
  { label: "cats purr", deg: 18 },
  { label: "kittens nap", deg: 34 },
  { label: "dogs bark", deg: 66 },
  { label: "birds sing", deg: 104 },
  { label: "cars need fuel", deg: 196 },
  { label: "invoice due", deg: 292 },
];
const K = 2;
const rad = (d: number) => (d * Math.PI) / 180;

export function SearchDemo() {
  const ref = useRef<HTMLDivElement>(null);
  const ms = useLoop(ref, PERIOD, 4000);
  const qDeg = 52 + 48 * Math.sin((ms / PERIOD) * Math.PI * 2);

  const ranked = ITEMS.map((it) => {
    const theta = Math.abs(((it.deg - qDeg + 540) % 360) - 180);
    return { ...it, theta, d: 1 - Math.cos(rad(theta)) };
  }).sort((a, b) => a.d - b.d);
  const top = new Set(ranked.slice(0, K).map((r) => r.label));
  const best = ranked[0];

  const cx = 200;
  const cy = 165;
  const R = 100;
  // y axis points down in SVG, so negate angles to keep counter-clockwise = positive
  const at = (deg: number, r = R): [number, number] => [cx + Math.cos(-rad(deg)) * r, cy + Math.sin(-rad(deg)) * r];

  return (
    <div ref={ref} className="grid grid-cols-1 md:grid-cols-[minmax(0,1fr)_minmax(0,1fr)] gap-4 items-start">
      <figure className="sheet">
        <svg viewBox="0 0 400 330" className="block w-full h-auto" style={{ fontFamily: "var(--font-mono)" }} role="img" aria-label="Stored vectors on a circle and a query vector; the two closest by angle are circled">
          <Stroke d={loop(cx, cy, R, R, 9)} w={1} dash="1 5" />
          <path d={`M${cx - R - 14} ${cy} H${cx + R + 14} M${cx} ${cy - R - 14} V${cy + R + 14}`} stroke="#000" strokeWidth="1" strokeDasharray="4 4" />
          {ITEMS.map((it, i) => {
            const tip = at(it.deg);
            const lab = at(it.deg, R + 14);
            const right = Math.cos(-rad(it.deg)) >= 0;
            return (
              <g key={it.label}>
                <Arrow a={[cx, cy]} b={tip} seed={60 + i} w={top.has(it.label) ? 1.8 : 1.1} dash={top.has(it.label) ? undefined : "3 3"} />
                <text x={lab[0]} y={lab[1] + 5} fontSize="17" textAnchor={right ? "start" : "end"} fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
                  {it.label}
                </text>
                {top.has(it.label) && <Stroke d={loop(tip[0], tip[1], 10, 10, 70 + i)} w={2} stroke={ORANGE} />}
              </g>
            );
          })}
          {/* query */}
          <Arrow a={[cx, cy]} b={at(qDeg, R * 0.92)} seed={80} w={3} color={ORANGE} />
          {(() => {
            // label the query beside its shaft, on the side away from the angle marker
            const m = at(qDeg, R * 0.62);
            const side = qDeg >= best.deg ? 1 : -1;
            const nx = -Math.sin(-rad(qDeg)) * 16 * side;
            const ny = Math.cos(-rad(qDeg)) * 16 * side;
            return (
              <text x={m[0] - nx} y={m[1] - ny + 4} fontSize="13" fontWeight="600" textAnchor="middle" fill={ORANGE}>
                query
              </text>
            );
          })()}
          {/* angle to the nearest */}
          <Stroke d={arc(cx, cy, 30, -rad(Math.max(qDeg, best.deg)), -rad(Math.min(qDeg, best.deg)))} w={1.8} stroke={ORANGE} />
          <text x={at((qDeg + best.deg) / 2, 42)[0]} y={at((qDeg + best.deg) / 2, 42)[1] + 4} fontSize="13" textAnchor="middle">
            θ
          </text>
        </svg>
      </figure>

      <div className="sheet px-4 py-3">
        <p className="text-[13px] mb-2">
          cosine distance, <span className="chip">{"<=>"}</span>
        </p>
        <p className="font-display text-[22px] mb-3">
          <span className="marker">d = 1 − cos θ</span>
        </p>
        <table className="w-full text-[13px] border-collapse">
          <thead>
            <tr className="border-b-[1.5px] border-ink text-left">
              <th className="py-1 font-semibold">rank</th>
              <th className="py-1 font-semibold">stored text</th>
              <th className="py-1 font-semibold text-right">θ</th>
              <th className="py-1 font-semibold text-right">d</th>
            </tr>
          </thead>
          <tbody>
            {ranked.map((r, i) => (
              <tr key={r.label} className={`border-b border-dashed border-ink ${i < K ? "font-semibold" : ""}`}>
                <td className="py-1">
                  {i + 1}
                  {i < K && <span className="ml-1 text-arkiv-orange">✓</span>}
                </td>
                <td className="py-1">{r.label}</td>
                <td className="py-1 text-right tabular-nums">{r.theta.toFixed(0)}°</td>
                <td className="py-1 text-right tabular-nums">{r.d.toFixed(3)}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <p className="mt-3 text-[12.5px]">
          With k = 2 the two smallest distances are returned. Real embeddings have hundreds of dimensions, so the angle is
          computed over all of them, but the arithmetic is the same.
        </p>
      </div>
    </div>
  );
}
