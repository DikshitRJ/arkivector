import { useRef } from "react";
import { BLUE, ORANGE } from "../ink/colors";
import { Arrow, Box, Stroke } from "../ink/Ink";
import { line } from "../ink/paths";
import { useLoop, useNarrow } from "./time";

/*
 * The central SDK -> arkivectord -> Arkiv diagram. Small dots travel along the links to show
 * the request directions. `markers` adds numbered orange tags (used by the build page).
 */

type R = { x: number; y: number; w: number; h: number };
type G = {
  vw: number;
  vh: number;
  fs: number;
  sdks: R[];
  daemon: R;
  parts: { r: R; t: string; s: string }[];
  arkiv: R;
  contract: R;
  blocks: R[];
  rest: [number, number][]; // where SDK links meet the daemon
};

const SDK_LABELS = [
  { t: "Python SDK", s: "pip install arkivector" },
  { t: "Node.js SDK", s: "@dikshitrj/arkivector" },
  { t: "curl / any HTTP", s: "REST on :8080" },
  { t: "AI agent", s: "MCP tools" },
];

function wide(): G {
  const parts = [
    { r: { x: 316, y: 92, w: 160, h: 58 }, t: "HTTP", s: "REST · MCP" },
    { r: { x: 488, y: 92, w: 160, h: 58 }, t: "writer", s: "ABI · sign · send" },
    { r: { x: 316, y: 162, w: 160, h: 58 }, t: "planner", s: "pgvector kernels" },
    { r: { x: 488, y: 162, w: 160, h: 58 }, t: "filters", s: "roaring bitmaps" },
    { r: { x: 316, y: 232, w: 332, h: 58 }, t: "store", s: "LMDB rows · mmap vector segments" },
    { r: { x: 316, y: 302, w: 332, h: 58 }, t: "syncer", s: "arkiv_query · eth_getLogs · reorgs · expiry" },
  ];
  return {
    vw: 960,
    vh: 400,
    fs: 1,
    sdks: [0, 1, 2, 3].map((i) => ({ x: 20, y: 62 + i * 76, w: 200, h: 58 })),
    daemon: { x: 300, y: 30, w: 364, h: 346 },
    parts,
    arkiv: { x: 740, y: 30, w: 200, h: 346 },
    contract: { x: 758, y: 98, w: 164, h: 52 },
    blocks: [0, 1, 2].map((i) => ({ x: 778, y: 184 + i * 64, w: 124, h: 44 })),
    rest: [[316, 121]],
  };
}

function tall(): G {
  const fs = 1.3;
  const parts = [
    { r: { x: 34, y: 286, w: 228, h: 70 }, t: "HTTP", s: "REST · MCP" },
    { r: { x: 278, y: 286, w: 228, h: 70 }, t: "writer", s: "ABI · sign · send" },
    { r: { x: 34, y: 368, w: 228, h: 70 }, t: "planner", s: "pgvector kernels" },
    { r: { x: 278, y: 368, w: 228, h: 70 }, t: "filters", s: "roaring bitmaps" },
    { r: { x: 34, y: 450, w: 472, h: 70 }, t: "store", s: "LMDB rows · mmap segments" },
    { r: { x: 34, y: 532, w: 472, h: 70 }, t: "syncer", s: "logs · reorgs · expiry" },
  ];
  return {
    vw: 540,
    vh: 860,
    fs,
    sdks: [0, 1, 2, 3].map((i) => ({ x: 18 + i * 127, y: 16, w: 123, h: 84 })),
    daemon: { x: 18, y: 222, w: 504, h: 396 },
    parts,
    arkiv: { x: 18, y: 668, w: 504, h: 176 },
    contract: { x: 34, y: 728, w: 200, h: 64 },
    blocks: [0, 1, 2].map((i) => ({ x: 262 + i * 86, y: 732, w: 74, h: 56 })),
    rest: [[148, 286]],
  };
}

const mono = { fontFamily: "var(--font-mono)" } as const;
const hand = { fontFamily: "var(--font-hand)" } as const;

function Tag({ n, x, y }: { n: number; x: number; y: number }) {
  return (
    <g>
      <circle cx={x} cy={y} r={13} fill={ORANGE} stroke="#000" strokeWidth={1.5} />
      <text x={x} y={y + 5} textAnchor="middle" fontSize={14} fontWeight={700} style={mono}>
        {n}
      </text>
    </g>
  );
}

export function ArchDiagram({ markers = false }: { markers?: boolean }) {
  const ref = useRef<HTMLDivElement>(null);
  const narrow = useNarrow();
  const g = narrow ? tall() : wide();
  const ms = useLoop(ref, 6000, 2500);
  const k = ms / 6000;
  const fs = g.fs;
  const http = g.parts[0].r;
  const writer = g.parts[1].r;
  const syncer = g.parts[5].r;

  // link paths
  const sdkLinks = g.sdks.map((s, i) =>
    narrow
      ? line([s.x + s.w / 2, s.y + s.h], [http.x + 40 + i * 50, http.y], 30 + i)
      : line([s.x + s.w, s.y + s.h / 2], [http.x, http.y + 14 + i * 10], 30 + i),
  );
  const txFrom: [number, number] = narrow ? [g.daemon.x + 120, g.daemon.y + g.daemon.h] : [writer.x + writer.w, writer.y + writer.h / 2];
  const txTo: [number, number] = narrow ? [g.contract.x + g.contract.w / 2, g.contract.y] : [g.contract.x, g.contract.y + g.contract.h / 2];
  const syncFrom: [number, number] = narrow ? [g.blocks[1].x + g.blocks[1].w / 2, g.blocks[1].y] : [g.blocks[1].x, g.blocks[1].y + g.blocks[1].h / 2];
  const syncTo: [number, number] = narrow ? [g.daemon.x + 380, g.daemon.y + g.daemon.h] : [syncer.x + syncer.w, syncer.y + syncer.h / 2];

  const along = (a: [number, number], b: [number, number], p: number): [number, number] => [a[0] + (b[0] - a[0]) * p, a[1] + (b[1] - a[1]) * p];

  return (
    <div ref={ref} className="sheet">
      <svg viewBox={`0 0 ${g.vw} ${g.vh}`} className="block w-full h-auto" role="img" aria-label="Your code calls arkivectord over REST through the Python or Node SDK; arkivectord sends signed transactions to Arkiv and syncs entities back">
        {/* SDK boxes */}
        {g.sdks.map((s, i) => (
          <g key={i}>
            <Box x={s.x} y={s.y} w={s.w} h={s.h} seed={10 + i} />
            <text x={s.x + (narrow ? 10 : 12)} y={s.y + 23 * fs} fontSize={(narrow ? 12.5 : 14) * fs} fontWeight={700} style={mono}>
              {narrow ? ["Python", "Node.js", "curl", "agent"][i] : SDK_LABELS[i].t}
            </text>
            {narrow ? (
              <text x={s.x + 10} y={s.y + 46 * fs} fontSize={11.5 * fs} style={mono}>
                {["pip", "npm", "REST", "MCP"][i]}
              </text>
            ) : (
              <text x={s.x + 12} y={s.y + 42 * fs} fontSize={11.5 * fs} style={mono}>
                {SDK_LABELS[i].s}
              </text>
            )}
          </g>
        ))}
        {sdkLinks.map((d, i) => (
          <Stroke key={i} d={d} w={1.2} dash="4 4" />
        ))}
        {/* request dots */}
        {g.sdks.map((s, i) => {
          const a: [number, number] = narrow ? [s.x + s.w / 2, s.y + s.h] : [s.x + s.w, s.y + s.h / 2];
          const b: [number, number] = narrow ? [http.x + 40 + i * 50, http.y] : [http.x, http.y + 14 + i * 10];
          const p = (k + i * 0.25) % 1;
          const [x, y] = along(a, b, p);
          return <circle key={i} cx={x} cy={y} r={3.5} fill="#000" />;
        })}

        {/* daemon */}
        <Box x={g.daemon.x} y={g.daemon.y} w={g.daemon.w} h={g.daemon.h} seed={3} sw={2.2} />
        <text x={g.daemon.x + 14} y={g.daemon.y + 28 * fs} fontSize={17 * fs} fontWeight={700} style={mono}>
          arkivectord
        </text>
        <text x={g.daemon.x + g.daemon.w - 14} y={g.daemon.y + 27 * fs} fontSize={11.5 * fs} textAnchor="end" style={mono}>
          C11 · pgvector 0.8.7 inside
        </text>
        {g.parts.map((p, i) => (
          <g key={p.t}>
            <Box x={p.r.x} y={p.r.y} w={p.r.w} h={p.r.h} seed={40 + i} sw={1.4} />
            <text x={p.r.x + 10} y={p.r.y + 22 * fs} fontSize={13.5 * fs} fontWeight={600} style={mono}>
              {p.t}
            </text>
            <text x={p.r.x + 10} y={p.r.y + 40 * fs} fontSize={11 * fs} style={mono}>
              {p.s}
            </text>
          </g>
        ))}

        {/* Arkiv */}
        <Box x={g.arkiv.x} y={g.arkiv.y} w={g.arkiv.w} h={g.arkiv.h} seed={4} sw={2.2} color={BLUE} />
        <text x={g.arkiv.x + 14} y={g.arkiv.y + 30 * fs} fontSize={19 * fs} fontWeight={700} fill={BLUE} style={{ fontFamily: "var(--font-display)" }}>
          [ ARKIV ]
        </text>
        <text
          x={narrow ? g.arkiv.x + g.arkiv.w - 14 : g.arkiv.x + 14}
          y={narrow ? g.arkiv.y + 30 * fs : g.arkiv.y + 50}
          fontSize={11.5 * fs}
          textAnchor={narrow ? "end" : "start"}
          style={mono}
        >
          {narrow ? "Rabdi testnet · chain 7794095" : "Rabdi · chain 7794095"}
        </text>
        <Box x={g.contract.x} y={g.contract.y} w={g.contract.w} h={g.contract.h} seed={5} color={BLUE} />
        <text x={g.contract.x + 10} y={g.contract.y + 22 * fs} fontSize={12 * fs} fontWeight={600} style={mono}>
          0x4400…0044
        </text>
        <text x={g.contract.x + 10} y={g.contract.y + 40 * fs} fontSize={11 * fs} style={mono}>
          execute(ops)
        </text>
        {g.blocks.map((b, i) => (
          <g key={i}>
            <Box x={b.x} y={b.y} w={b.w} h={b.h} seed={60 + i} sw={1.4} />
            <text x={b.x + b.w / 2} y={b.y + b.h / 2 + 4 * fs} fontSize={11.5 * fs} textAnchor="middle" fontWeight={600} style={mono}>
              {`block ${["N-1", "N", "N+1"][i]}`}
            </text>
          </g>
        ))}

        {/* writes go out, entities come back */}
        <Arrow a={txFrom} b={txTo} seed={70} w={1.8} color={ORANGE} />
        <circle cx={along(txFrom, txTo, k)[0]} cy={along(txFrom, txTo, k)[1]} r={4} fill={ORANGE} />
        <text
          x={narrow ? txFrom[0] + 10 : (txFrom[0] + txTo[0]) / 2}
          y={narrow ? (txFrom[1] + txTo[1]) / 2 : txFrom[1] - 12}
          fontSize={18 * fs}
          textAnchor={narrow ? "start" : "middle"}
          fill={BLUE}
          style={hand}
        >
          signed tx
        </text>
        <Arrow a={syncFrom} b={syncTo} seed={71} w={1.8} />
        <circle cx={along(syncFrom, syncTo, (k + 0.5) % 1)[0]} cy={along(syncFrom, syncTo, (k + 0.5) % 1)[1]} r={4} fill="#000" />
        <text
          x={narrow ? syncFrom[0] + 10 : (syncFrom[0] + syncTo[0]) / 2 + 6}
          y={narrow ? (syncFrom[1] + syncTo[1]) / 2 - 6 : syncTo[1] + 30}
          fontSize={18 * fs}
          textAnchor={narrow ? "start" : "middle"}
          fill={BLUE}
          style={hand}
        >
          entities, logs
        </text>
        {!narrow && (
          <text x={(g.sdks[0].x + g.sdks[0].w + http.x) / 2 - 4} y={52} fontSize={18} textAnchor="middle" fill={BLUE} style={hand}>
            REST :8080
          </text>
        )}

        {markers && (
          <g>
            <Tag n={1} x={g.sdks[0].x + g.sdks[0].w - 8} y={g.sdks[0].y + 8} />
            <Tag n={2} x={g.sdks[1].x + g.sdks[1].w - 8} y={g.sdks[1].y + 8} />
            <Tag n={3} x={g.daemon.x + g.daemon.w - 40} y={g.daemon.y} />
            <Tag n={4} x={g.daemon.x + g.daemon.w - 8} y={g.daemon.y} />
            <Tag n={5} x={g.arkiv.x + g.arkiv.w - 8} y={g.arkiv.y + 8} />
          </g>
        )}
      </svg>
    </div>
  );
}
