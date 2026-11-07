import { useRef } from "react";
import { BLUE, ORANGE } from "../ink/colors";
import { Arrow, Box, InkDefs, Stroke } from "../ink/Ink";
import { line, loop, rect } from "../ink/paths";
import { ease, easeOut, lerp, project, seg, useLoop } from "./time";

/*
 * Two panels on one clock (12 s loop):
 *   left:  a sentence is typed, split into tokens, sent through an embedding model,
 *          and comes out as a column of numbers.
 *   right: a rotating 3D vector space where that column lands as a new point, next to
 *          the closest existing one.
 */

const PERIOD = 12000;
const SENTENCE = "cats purr at night";
const TOKENS = [
  { s: "cats", id: 9246 },
  { s: "purr", id: 52814 },
  { s: "at", id: 520 },
  { s: "night", id: 3814 },
];
const NUMS = ["0.12", "-0.48", "0.91", "0.03", "-0.27", "0.66"];

type V3 = [number, number, number];
const STORED: { label: string; v: V3; seed: number }[] = [
  { label: "kittens nap", v: [0.62, 0.55, 0.42], seed: 11 },
  { label: "dogs bark", v: [0.05, 0.88, -0.35], seed: 12 },
  { label: "cars need fuel", v: [-0.7, -0.15, 0.62], seed: 13 },
  { label: "invoice due friday", v: [-0.2, -0.75, -0.55], seed: 14 },
];
const NEW: V3 = [0.72, 0.52, 0.3];

const svgText = { fontFamily: "var(--font-mono)" } as const;

export function HeroDemo() {
  const ref = useRef<HTMLDivElement>(null);
  const ms = useLoop(ref, PERIOD, 10500);
  const s = ms / 1000;
  // erase everything during the last 0.7 s, then start again
  const erase = 1 - seg(s, 11.3, 12);
  return (
    <div ref={ref} className="grid grid-cols-1 md:grid-cols-2 gap-4">
      <figure className="sheet">
        <TextPanel s={s} erase={erase} />
        <figcaption className="border-t border-dashed border-ink px-3 py-2 text-[12.5px]">
          text in, numbers out. token ids and values are illustrative; real embeddings have 384 to 3072 numbers.
        </figcaption>
      </figure>
      <figure className="sheet">
        <SpacePanel s={s} erase={erase} />
        <figcaption className="border-t border-dashed border-ink px-3 py-2 text-[12.5px]">
          the same numbers as a point. similar meaning ends up nearby.
        </figcaption>
      </figure>
    </div>
  );
}

function TextPanel({ s, erase }: { s: number; erase: number }) {
  const typed = Math.round(SENTENCE.length * seg(s, 0.2, 2.0) * erase);
  const caretOn = s < 2.3 && Math.floor(s * 2.5) % 2 === 0;
  const pTok = seg(s, 2.3, 3.5) * erase;
  const pModel = seg(s, 3.6, 4.6) * erase;
  const pNums = seg(s, 4.8, 6.6) * erase;
  const pDot = seg(s, 6.8, 7.6) * erase;

  const tokX = [24, 108, 192, 252];
  const tokW = [72, 72, 48, 84];
  return (
    <svg viewBox="0 0 360 330" className="block w-full h-auto" style={svgText} role="img" aria-label="A sentence is split into tokens and turned into a list of numbers by an embedding model">
      <InkDefs />
      {/* ruled line + typed sentence */}
      <path d="M18 52 H342" stroke="#000" strokeWidth="1" strokeDasharray="1 4" />
      <text x="24" y="46" fontSize="20">
        {SENTENCE.slice(0, typed)}
        {caretOn ? "|" : ""}
      </text>

      {/* tokens */}
      {TOKENS.map((t, i) => {
        const p = seg(pTok, i * 0.18, i * 0.18 + 0.5);
        return (
          <g key={t.s}>
            <Box x={tokX[i]} y={70} w={tokW[i]} h={30} seed={i + 2} p={p} />
            {p > 0.6 && (
              <>
                <text x={tokX[i] + tokW[i] / 2} y={90} fontSize="14" textAnchor="middle">{t.s}</text>
                <text x={tokX[i] + tokW[i] / 2} y={118} fontSize="12.5" textAnchor="middle">{t.id}</text>
              </>
            )}
          </g>
        );
      })}

      {/* into the model */}
      {TOKENS.map((_, i) => (
        <Arrow key={i} a={[tokX[i] + tokW[i] / 2, 124]} b={[180 + (i - 1.5) * 14, 148]} seed={20 + i} p={seg(pModel, 0, 0.6)} w={1.2} />
      ))}
      <Stroke d={rect(90, 150, 180, 36, 31)} p={seg(pModel, 0.3, 1)} dash="5 4" />
      {pModel > 0.8 && (
        <text x="180" y="173" fontSize="14" textAnchor="middle">embedding model</text>
      )}
      <Arrow a={[180, 188]} b={[180, 208]} seed={33} p={seg(pNums, 0, 0.2)} />

      {/* column vector */}
      <Stroke d={`M150 212 h-8 v104 h8`} p={seg(pNums, 0.1, 0.4)} w={1.8} />
      <Stroke d={`M210 212 h8 v104 h-8`} p={seg(pNums, 0.1, 0.4)} w={1.8} />
      {NUMS.map((n, i) => {
        const p = seg(pNums, 0.25 + i * 0.1, 0.35 + i * 0.1);
        if (p <= 0) return null;
        return (
          <text key={i} x="200" y={228 + i * 14} fontSize="13" textAnchor="end">
            {n.slice(0, Math.max(1, Math.round(n.length * p)))}
          </text>
        );
      })}
      {pNums > 0.95 && (
        <text x="180" y="315" fontSize="13" textAnchor="middle">...</text>
      )}

      {/* the column leaves as a point, heading to the right panel */}
      {pDot > 0 && (
        <g>
          <Stroke d={line([224, 264], [lerp(232, 338, easeOut(pDot)), lerp(264, 247, easeOut(pDot))], 41)} w={1.4} dash="2 4" stroke={ORANGE} />
          <circle cx={lerp(232, 344, easeOut(pDot))} cy={lerp(264, 246, easeOut(pDot))} r={5.5} fill={ORANGE} />
        </g>
      )}
      <text x="250" y="232" fontSize="20" fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
        {pNums > 0.9 ? "one vector" : ""}
      </text>
    </svg>
  );
}

function SpacePanel({ s, erase }: { s: number; erase: number }) {
  const yaw = s * 0.32 + 0.6;
  const pitch = 0.42;
  const cx = 180;
  const cy = 170;
  const R = 108;
  const P = (v: V3) => project(v, yaw, pitch, R, cx, cy);
  const pAxes = seg(s, 0, 1.2) * erase;
  const pArrive = ease(seg(s, 7.3, 8.6)) * erase;
  const pNear = seg(s, 8.8, 9.8) * erase;

  const axes: { v: V3; l: string }[] = [
    { v: [1.15, 0, 0], l: "x" },
    { v: [0, 1.15, 0], l: "y" },
    { v: [0, 0, 1.15], l: "z" },
  ];
  // equator ellipse of the unit sphere
  const eq: string[] = [];
  for (let i = 0; i <= 48; i++) {
    const a = (i / 48) * Math.PI * 2;
    const [x, y] = P([Math.cos(a), 0, Math.sin(a)]);
    eq.push(`${i ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`);
  }
  const o = P([0, 0, 0]);
  const newPt = P(NEW);
  const nearPt = P(STORED[0].v);
  const arrivedAt: [number, number] = [lerp(354, newPt[0], pArrive), lerp(246, newPt[1], pArrive)];

  return (
    <svg viewBox="0 0 360 330" className="block w-full h-auto" style={svgText} role="img" aria-label="A rotating three dimensional vector space where similar sentences are close together">
      {/* unit sphere outline and equator */}
      <Stroke d={`M${cx - R} ${cy} a${R} ${R} 0 1 0 ${2 * R} 0 a${R} ${R} 0 1 0 ${-2 * R} 0`} p={pAxes} w={1} dash="1 5" />
      <Stroke d={eq.join(" ")} p={pAxes} w={1} dash="1 5" />
      {axes.map((a, i) => {
        const [x, y] = P(a.v);
        const [nx, ny] = P([-a.v[0] * 0.5, -a.v[1] * 0.5, -a.v[2] * 0.5]);
        return (
          <g key={a.l}>
            <Stroke d={`M${nx} ${ny} L${x} ${y}`} p={pAxes} w={1} dash="4 4" />
            {pAxes > 0.9 && (
              <text x={x + 4} y={y - 4} fontSize="13">{a.l}</text>
            )}
            {i === 0 && null}
          </g>
        );
      })}

      {/* stored vectors: points behind the screen plane are drawn dashed */}
      {STORED.map((it) => {
        const [x, y, z] = P(it.v);
        const behind = z < 0;
        return (
          <g key={it.label}>
            <Arrow a={[o[0], o[1]]} b={[x, y]} seed={it.seed} p={seg(pAxes, 0.4, 1)} w={1.2} dash={behind ? "3 3" : undefined} />
            {pAxes > 0.95 && (
              <text x={x + (x > cx ? 8 : -8)} y={y + 4} fontSize="18" textAnchor={x > cx ? "start" : "end"} fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
                {it.label}
              </text>
            )}
          </g>
        );
      })}

      {/* the new vector arrives */}
      {pArrive > 0 && (
        <g>
          {pArrive >= 1 && <Arrow a={[o[0], o[1]]} b={[newPt[0], newPt[1]]} seed={7} w={2.6} color={ORANGE} />}
          <circle cx={arrivedAt[0]} cy={arrivedAt[1]} r={5.5} fill={ORANGE} />
        </g>
      )}
      {pArrive >= 1 && (
        <g>
          <circle cx="26" cy="24" r="5.5" fill={ORANGE} />
          <text x="38" y="29" fontSize="13" fontWeight="600">cats purr at night (new)</text>
        </g>
      )}

      {/* nearest neighbour */}
      {pNear > 0 && (
        <g>
          <Stroke d={line([newPt[0], newPt[1]], [nearPt[0], nearPt[1]], 51)} p={pNear} w={1.4} dash="2 3" stroke={ORANGE} />
          <Stroke d={loop(nearPt[0], nearPt[1], 12, 12, 52)} p={pNear} w={2} stroke={ORANGE} />
          {pNear > 0.9 && (
            <text x="24" y="314" fontSize="20" fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
              nearest stored vector: "kittens nap"
            </text>
          )}
        </g>
      )}
    </svg>
  );
}
