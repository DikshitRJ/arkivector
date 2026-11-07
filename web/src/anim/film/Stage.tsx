import type { ReactNode } from "react";
import { BLUE, ORANGE } from "../../ink/colors";
import { Arrow, Box, InkDefs, Stroke } from "../../ink/Ink";
import { line, loop, rect } from "../../ink/paths";
import { ease, lerp, seg } from "../time";
import { bottom, cx, left, mid, right, top, type Layout, type R } from "./layout";
import { N } from "./script";

type P2 = [number, number];
const MONO = { fontFamily: "var(--font-mono)" } as const;
const HAND = { fontFamily: "var(--font-hand)" } as const;

/* ---------------- small drawing helpers ---------------- */

function T({
  x,
  y,
  s,
  children,
  w = 400,
  a = "start",
  fill = "#000",
  hand = false,
}: {
  x: number;
  y: number;
  s: number;
  children: ReactNode;
  w?: number;
  a?: "start" | "middle" | "end";
  fill?: string;
  hand?: boolean;
}) {
  return (
    <text x={x} y={y} fontSize={s} fontWeight={w} textAnchor={a} fill={fill} style={hand ? HAND : MONO}>
      {children}
    </text>
  );
}

/** A labelled compartment of the daemon (or the app / chain frame). */
function Part({ r, title, sub, p, seed, L }: { r: R; title: string; sub?: string; p: number; seed: number; L: Layout }) {
  return (
    <g>
      <Box x={r.x} y={r.y} w={r.w} h={r.h} seed={seed} p={p} />
      {p >= 1 && (
        <>
          <T x={r.x + 10} y={r.y + 20 * L.fs} s={14 * L.fs} w={600}>
            {title}
          </T>
          {sub && (
            <T x={r.x + 10} y={r.y + 37 * L.fs} s={11.5 * L.fs}>
              {sub}
            </T>
          )}
        </>
      )}
    </g>
  );
}

/** A slip of paper travelling from a to b while k goes 0 to 1. */
function Packet({ a, b, k, label, L, color = "#000", seed = 1 }: { a: P2; b: P2; k: number; label: string; L: Layout; color?: string; seed?: number }) {
  if (k <= 0 || k >= 1) return null;
  const e = ease(k);
  const x = lerp(a[0], b[0], e);
  const y = lerp(a[1], b[1], e);
  const s = 11.5 * L.fs;
  const w = label.length * s * 0.62 + 16;
  const h = s + 12;
  return (
    <g>
      <Stroke d={line(a, [x, y], seed)} w={1.2} dash="2 4" stroke={color} />
      <rect x={x - w / 2} y={y - h / 2} width={w} height={h} fill="#fff" />
      <Stroke d={rect(x - w / 2, y - h / 2, w, h, seed + 4)} w={1.6} stroke={color} />
      <T x={x} y={y + s * 0.36} s={s} a="middle" w={500}>
        {label}
      </T>
    </g>
  );
}

/** Two-leg packet through a waypoint. */
function Packet2({ a, m, b, k, label, L, color, seed }: { a: P2; m: P2; b: P2; k: number; label: string; L: Layout; color?: string; seed?: number }) {
  if (k <= 0 || k >= 1) return null;
  return k < 0.5 ? (
    <Packet a={a} b={m} k={k * 2} label={label} L={L} color={color} seed={seed} />
  ) : (
    <Packet a={m} b={b} k={(k - 0.5) * 2} label={label} L={L} color={color} seed={seed} />
  );
}

/* ---------------- world state as a function of time ---------------- */

type Blk = { n: string; at: number; ours?: boolean; orphanAt?: number };

function chainBlocks(t: number): Blk[] {
  const out: Blk[] = [];
  for (let i = 5; i >= 1; i--) out.push({ n: String(N - i), at: -10 });
  if (t >= 25) out.push({ n: String(N), at: 25, ours: true });
  [57.4, 58.2, 59, 59.8].forEach((at, i) => {
    if (t >= at) out.push({ n: String(N + 1 + i), at });
  });
  if (t >= 65) out.push({ n: String(N + 5), at: 65, orphanAt: 66.8 });
  if (t >= 70.6) out.push({ n: `${N + 5}′`, at: 70.6 });
  return out;
}

type Row = { name: string; at: number; goneAt?: number; note?: string };
const ROWS: Row[] = [
  { name: "dog", at: -10 },
  { name: "car", at: -10 },
  { name: "fish", at: -10, goneAt: 61.4, note: `expires ${N + 4}` },
  { name: "cat", at: 34.6 },
  { name: "owl", at: 65.6, goneAt: 69.8 },
  { name: "bee", at: 71.2 },
];

function asOf(t: number): number {
  if (t < 35.2) return N - 1;
  if (t < 57.4) return N;
  const blocks = chainBlocks(t).filter((b) => !b.n.endsWith("′"));
  return Number(blocks[blocks.length - 1].n);
}

/* ---------------- the stage ---------------- */

export function Stage({ t, L }: { t: number; L: Layout }) {
  if (t >= 74) return <Finale t={t} L={L} />;

  const fs = L.fs;
  const draw = (a: number, b: number) => seg(t, a, b);

  // chapter 1: draw the parts
  const pApp = draw(0.3, 1.6);
  const pDaemon = draw(1.8, 3.2);
  const pPart = (i: number) => draw(2.6 + i * 0.35, 3.4 + i * 0.35);
  const pChain = draw(4.6, 6.2);

  const searching = t >= 41 && t < 56;
  const bitsOn = t >= 43.6;

  return (
    <svg viewBox={`0 0 ${L.vw} ${L.vh}`} className="block w-full h-auto select-none" role="img" aria-label="Animated diagram of the Arkivector architecture">
      <InkDefs />

      {/* ---------- your app ---------- */}
      <Box x={L.app.x} y={L.app.y} w={L.app.w} h={L.app.h} seed={2} p={pApp} />
      {pApp >= 1 && (
        <>
          <T x={L.app.x + 12} y={L.app.y + 24 * fs} s={15 * fs} w={700}>
            your app
          </T>
          <T x={L.app.x + 12} y={L.app.y + 44 * fs} s={11.5 * fs}>
            Python / Node SDK
          </T>
          {!L.vertical ? null : (
            <T x={L.app.x + 12} y={L.app.y + 62 * fs} s={11.5 * fs}>
              or curl, MCP
            </T>
          )}
        </>
      )}

      {/* ---------- the daemon ---------- */}
      <Box x={L.daemon.x} y={L.daemon.y} w={L.daemon.w} h={L.daemon.h} seed={3} p={pDaemon} sw={2.2} />
      {pDaemon >= 1 && (
        <T x={L.daemon.x + 14} y={L.daemon.y + 28 * fs} s={17 * fs} w={700}>
          arkivectord
        </T>
      )}
      {pDaemon >= 1 && (
        <T x={L.daemon.x + L.daemon.w - 14} y={L.daemon.y + 27 * fs} s={11.5 * fs} a="end">
          C11 · pgvector 0.8.7 inside
        </T>
      )}
      <Part r={L.http} title="HTTP" sub="REST, libuv loop" p={pPart(0)} seed={10} L={L} />
      <Part r={L.writer} title="writer" sub="ABI · sign · send" p={pPart(1)} seed={11} L={L} />
      <Part r={L.planner} title="planner" sub="pgvector kernels" p={pPart(2)} seed={12} L={L} />
      <Part r={L.filter} title="filters" sub="roaring bitmaps" p={pPart(3)} seed={13} L={L} />
      <Part r={L.store} title="store" sub="LMDB rows · mmap segments" p={pPart(4)} seed={14} L={L} />
      <Part r={L.syncer} title="syncer" sub="logs · reorgs · expiry" p={pPart(5)} seed={15} L={L} />

      {/* ---------- the chain ---------- */}
      <Box x={L.chain.x} y={L.chain.y} w={L.chain.w} h={L.chain.h} seed={4} p={pChain} sw={2.2} />
      {pChain >= 1 && (
        <>
          <T x={L.chain.x + 14} y={L.chain.y + 30 * fs} s={19 * fs} w={700} fill={BLUE}>
            [ ARKIV ]
          </T>
          <T
            x={L.vertical ? L.chain.x + 14 : L.chain.x + L.chain.w - 14}
            y={L.vertical ? L.chain.y + 48 * fs : L.chain.y + 30 * fs}
            s={11.5 * fs}
            a={L.vertical ? "start" : "end"}
          >
            Rabdi testnet
          </T>
        </>
      )}
      <Chain t={t} L={L} p={pChain} />

      {/* REST link + chain link (ink rails) */}
      {pDaemon >= 1 && <Stroke d={L.vertical ? line(right(L.app), left(L.daemon), 90) : line(bottom(L.app), top(L.daemon), 90)} w={1.2} dash="4 4" />}
      {pChain >= 1 && <Stroke d={L.vertical ? line(right(L.daemon), left(L.chain), 91) : line(bottom(L.daemon), top(L.chain), 91)} w={1.2} dash="4 4" />}

      {/* ---------- store contents ---------- */}
      {t >= 6 && <StoreRows t={t} L={L} />}

      {/* ---------- chapter 2: a write ---------- */}
      <Packet a={L.vertical ? right(L.app) : bottom(L.app)} b={L.vertical ? left(L.http) : top(L.http)} k={draw(9.4, 10.8)} label="upsert cat" L={L} seed={20} />
      <Packet a={right(L.http)} b={left(L.writer)} k={draw(10.8, 11.6)} label="cat" L={L} seed={21} />
      {t >= 11.6 && t < 22.6 && <Slip t={t} L={L} />}

      {/* ---------- chapter 3: signed and sent ---------- */}
      <Packet
        a={L.vertical ? right(L.writer) : bottom(L.syncer)}
        b={mid(slotOf(L, String(N), t + 0.01))}
        k={draw(22.6, 25)}
        label="tx 0x1945…f874"
        L={L}
        color={ORANGE}
        seed={30}
      />
      <Packet2
        a={mid(slotOf(L, String(N), t))}
        m={L.vertical ? right(L.writer) : bottom(L.syncer)}
        b={L.vertical ? right(L.app) : bottom(L.app)}
        k={draw(27.4, 30.4)}
        label={`receipt · ${N}`}
        L={L}
        seed={31}
      />

      {/* ---------- chapter 4: synced back ---------- */}
      {t >= 31 && t < 34.6 && (
        <g>
          <Arrow
            a={L.vertical ? left(L.chain) : top(L.chain)}
            b={L.vertical ? right(L.syncer) : [cx(L.syncer), L.syncer.y + L.syncer.h]}
            seed={40}
            p={draw(31, 32.4)}
            w={1.8}
            color={ORANGE}
          />
          {t >= 32.4 && (
            <T x={L.syncer.x + L.syncer.w - 12} y={L.syncer.y + L.syncer.h - 12} s={13 * fs} a="end" w={600} fill={ORANGE}>
              eth_getLogs → EntityCreated
            </T>
          )}
        </g>
      )}
      <Packet a={top(L.syncer)} b={bottom(L.store)} k={draw(33.6, 34.6)} label="cat" L={L} color={ORANGE} seed={41} />
      <Packet
        a={L.vertical ? right(L.app) : bottom(L.app)}
        b={L.vertical ? left(L.http) : top(L.http)}
        k={draw(38.6, 40.6)}
        label={`read · min_block ${N}`}
        L={L}
        seed={42}
      />

      {/* ---------- chapter 5: a search ---------- */}
      <Packet a={L.vertical ? right(L.app) : bottom(L.app)} b={L.vertical ? left(L.http) : top(L.http)} k={draw(41, 42.4)} label="search k=2 pets" L={L} seed={50} />
      <Packet a={bottom(L.http)} b={top(L.planner)} k={draw(42.4, 43.4)} label="query" L={L} seed={51} />
      {(searching || (t >= 56 && t < 74)) && bitsOn && <Bitmaps t={t} L={L} />}
      {searching && t >= 47.6 && <Scores t={t} L={L} />}
      <Packet2
        a={top(L.planner)}
        m={L.vertical ? left(L.http) : top(L.http)}
        b={L.vertical ? right(L.app) : bottom(L.app)}
        k={draw(52.8, 55.4)}
        label={`cat .0012 · dog .0018 @${N}`}
        L={L}
        color={ORANGE}
        seed={52}
      />

      {/* ---------- chapter 7: hash ring ---------- */}
      {t >= 66.6 && t < 74 && <HashRing t={t} L={L} />}

      {/* chapter heading in the corner, like a page header */}
      <T x={L.vw - 12} y={L.vh - 10} s={11 * fs} a="end">
        {Math.floor(t)}s
      </T>
    </svg>
  );
}

/* ---------------- pieces ---------------- */

function slotOf(L: Layout, n: string, t: number): R {
  const bl = chainBlocks(t);
  const vis = bl.slice(-L.slots);
  const i = vis.findIndex((b) => b.n === n);
  return L.block(i < 0 ? vis.length : i);
}

function Chain({ t, L, p }: { t: number; L: Layout; p: number }) {
  if (p <= 0) return null;
  const vis = chainBlocks(t).slice(-L.slots);
  const fs = L.fs;
  return (
    <g>
      {vis.map((b, i) => {
        const r = L.block(i);
        const pb = b.at < 0 ? p : seg(t, b.at, b.at + 0.6);
        const orphan = b.orphanAt !== undefined && t >= b.orphanAt;
        return (
          <g key={b.n}>
            <Box x={r.x} y={r.y} w={r.w} h={r.h} seed={100 + Number(b.n.replace("′", "1")) % 50} p={pb} hatch={orphan} color={b.ours ? ORANGE : "#000"} sw={b.ours ? 2.2 : 1.6} />
            {pb >= 1 && (
              <>
                <rect x={r.x + 6} y={r.y + r.h / 2 - 9 * fs} width={r.w - 12} height={15 * fs} fill="#fff" />
                <T x={r.x + r.w / 2} y={r.y + r.h / 2 + 2 * fs} s={12 * fs} a="middle" w={600}>
                  {b.n}
                </T>
              </>
            )}
            {pb >= 1 && b.ours && <circle cx={r.x + r.w - 10} cy={r.y + 10} r={4 * fs} fill={ORANGE} />}
            {orphan && <Stroke d={line([r.x + 4, r.y + 4], [r.x + r.w - 4, r.y + r.h - 4], 7)} w={2.2} stroke={ORANGE} />}
            {i > 0 && pb >= 1 && (
              <Stroke
                d={
                  L.vertical
                    ? line([cx(r), r.y - 18 + 2], [cx(r), r.y - 2], 200 + i)
                    : line([r.x - 12 + 1, r.y + r.h / 2], [r.x - 1, r.y + r.h / 2], 200 + i)
                }
                w={1.4}
              />
            )}
          </g>
        );
      })}
    </g>
  );
}

function StoreRows({ t, L }: { t: number; L: Layout }) {
  const fs = L.fs;
  const r = L.store;
  const cw = (r.w - 24 - 5 * 8) / 6;
  const ch = 26 * fs;
  const y = r.y + r.h - ch - 12;
  return (
    <g>
      <T x={r.x + r.w - 10} y={r.y + 20 * fs} s={11.5 * fs} a="end" w={t >= 35.2 && t < 37 ? 700 : 400} fill={t >= 35.2 && t < 37 ? ORANGE : "#000"}>
        as_of_block {asOf(t)}
      </T>
      {ROWS.map((row, i) => {
        if (t < row.at) return null;
        const x = r.x + 12 + i * (cw + 8);
        const p = row.at < 0 ? 1 : seg(t, row.at, row.at + 0.5);
        const gone = row.goneAt !== undefined && t >= row.goneAt;
        const fresh = row.at > 0 && t - row.at < 2.5;
        return (
          <g key={row.name}>
            <Box x={x} y={y} w={cw} h={ch} seed={300 + i} p={p} hatch={gone} color={fresh ? ORANGE : "#000"} sw={fresh ? 2 : 1.4} />
            {p >= 1 && (
              <>
                {gone && <rect x={x + cw / 2 - 18 * fs} y={y + ch / 2 - 8 * fs} width={36 * fs} height={15 * fs} fill="#fff" />}
                <T x={x + cw / 2} y={y + ch / 2 + 4.5 * fs} s={12.5 * fs} a="middle" w={600}>
                  {row.name}
                </T>
              </>
            )}
            {gone && <Stroke d={line([x + 3, y + ch / 2], [x + cw - 3, y + ch / 2], 9 + i)} w={2} stroke={ORANGE} />}
            {row.note && t >= 58.6 && t < 64 && (
              <T x={x + cw / 2} y={y - 6} s={17 * fs} a="middle" fill={BLUE} hand>
                {row.note}
              </T>
            )}
          </g>
        );
      })}
    </g>
  );
}

/** The vector-bytes slip shown while the writer encodes. */
function Slip({ t, L }: { t: number; L: Layout }) {
  const fs = L.fs;
  const r: R = {
    x: L.planner.x + 10,
    y: L.planner.y + 8,
    w: L.filter.x + L.filter.w - L.planner.x - 20,
    h: L.planner.h - 16,
  };
  const p = seg(t, 11.6, 12.2);
  const hex1 = "41 56 12 00 04 00 00 00";
  const hex2 = "00 3c 00 00 00 00 00 00";
  const k1 = Math.round(hex1.length * seg(t, 12.2, 13.4));
  const k2 = Math.round(hex2.length * seg(t, 13.4, 14.4));
  const s = 13 * fs;
  return (
    <g>
      <Box x={r.x} y={r.y} w={r.w} h={r.h} seed={60} p={p} color={ORANGE} sw={2} />
      {p >= 1 && (
        <>
          <T x={r.x + 12} y={r.y + 20 * fs} s={11.5 * fs} w={600}>
            {t < 15.2 ? "VectorBytes, halfvec [1, 0, 0, 0]" : "execute((uint8,bytes)[])"}
          </T>
          {t < 15.2 ? (
            <>
              <T x={r.x + 12} y={r.y + 44 * fs} s={s} w={500}>
                {hex1.slice(0, k1)}
              </T>
              <T x={r.x + 12} y={r.y + 64 * fs} s={s} w={500}>
                {hex2.slice(0, k2)}
              </T>
              {t >= 14.4 && (
                <T x={r.x + r.w - 12} y={r.y + r.h - 10} s={16 * fs} a="end" fill={BLUE} hand>
                  'AV', type 2 = halfvec, dim 4, then 1.0 as binary16
                </T>
              )}
            </>
          ) : (
            <>
              <T x={r.x + 12} y={r.y + 44 * fs} s={s} w={500}>
                CREATE  arkiv.schema, av.id,
              </T>
              <T x={r.x + 12} y={r.y + 64 * fs} s={s} w={500}>
                        av.vec, av.doc, topic
              </T>
              {t >= 20 && (
                <g>
                  <Stroke
                    d={`M${r.x + r.w - 150 * fs} ${r.y + r.h - 18} c 10 -22, 18 -22, 20 0 s 10 18, 22 -2 s 12 -20, 18 0 s 14 10, 30 -6 s 18 4, 40 -2`}
                    p={seg(t, 20, 21.6)}
                    w={2}
                    stroke={BLUE}
                  />
                  {t >= 21.6 && (
                    <T x={r.x + 12} y={r.y + r.h - 10} s={11.5 * fs} w={600} fill={ORANGE}>
                      signed · EIP-1559 · secp256k1
                    </T>
                  )}
                </g>
              )}
            </>
          )}
        </>
      )}
    </g>
  );
}

const PETS = [1, 0, 1, 1, 0, 1, 0, 0];
const LIVE = [1, 1, 1, 1, 1, 0, 1, 1];

function Bitmaps({ t, L }: { t: number; L: Layout }) {
  const fs = L.fs;
  const r = L.filter;
  const cs = Math.min(15 * fs, (r.w - 70 * fs) / 8 - 2);
  const x0 = r.x + 58 * fs;
  const rows: { label: string; bits: number[]; at: number; hot?: boolean }[] = [
    { label: "pets", bits: PETS, at: 43.6 },
    { label: "live", bits: LIVE.map((b, i) => (i === 2 && t >= 61.4 ? 0 : b)), at: 45.6 },
    { label: "AND", bits: PETS.map((b, i) => b & LIVE[i] & (i === 2 && t >= 61.4 ? 0 : 1)), at: 46.6, hot: true },
  ];
  return (
    <g>
      <rect x={r.x + 3} y={r.y + 44 * fs} width={r.w - 6} height={r.h - 48 * fs} fill="#fff" />
      {rows.map((row, ri) => {
        if (t < row.at) return null;
        const y = r.y + 52 * fs + ri * (cs + 9 * fs);
        return (
          <g key={row.label}>
            <T x={r.x + 10} y={y + cs * 0.8} s={11.5 * fs} w={row.hot ? 700 : 400}>
              {row.label}
            </T>
            {row.bits.map((b, i) => {
              const on = seg(t, row.at + i * 0.06, row.at + i * 0.06 + 0.2) >= 1 && b === 1;
              return (
                <rect
                  key={i}
                  x={x0 + i * (cs + 2)}
                  y={y}
                  width={cs}
                  height={cs}
                  fill={on ? (row.hot ? ORANGE : "#000") : "#fff"}
                  stroke="#000"
                  strokeWidth={1}
                />
              );
            })}
          </g>
        );
      })}
      {t >= 61.4 && t < 64 && (
        <T x={x0 + 2 * (cs + 2) + cs / 2} y={r.y + 52 * fs + 3 * (cs + 9 * fs) + 4 * fs} s={16 * fs} a="middle" fill={BLUE} hand>
          ↑ fish
        </T>
      )}
    </g>
  );
}

function Scores({ t, L }: { t: number; L: Layout }) {
  const fs = L.fs;
  const r = L.planner;
  const rows = [
    { id: "cat", d: "0.0012" },
    { id: "dog", d: "0.0018" },
    { id: "fish", d: "0.0311" },
  ];
  return (
    <g>
      <rect x={r.x + 3} y={r.y + 44 * fs} width={r.w - 6} height={r.h - 48 * fs} fill="#fff" />
      <T x={r.x + r.w - 10} y={r.y + 20 * fs} s={11.5 * fs} a="end" w={600}>
        {"<=> cosine"}
      </T>
      {rows.map((row, i) => {
        const at = 47.8 + i * 0.7;
        if (t < at) return null;
        const y = r.y + 62 * fs + i * 21 * fs;
        const chosen = i < 2 && t >= 50.6;
        return (
          <g key={row.id}>
            <T x={r.x + 16} y={y} s={13 * fs} w={chosen ? 700 : 400}>
              {row.id.padEnd(5, " ")} {row.d}
            </T>
            {chosen && <Stroke d={loop(r.x + 16 + 50 * fs, y - 5 * fs, 58 * fs, 12 * fs, 400 + i)} p={seg(t, 50.6 + i * 0.3, 51.2 + i * 0.3)} w={1.8} stroke={ORANGE} />}
          </g>
        );
      })}
      {t >= 51.4 && (
        <T x={r.x + r.w - 12} y={r.y + r.h - 10} s={17 * fs} a="end" fill={BLUE} hand>
          k = 2
        </T>
      )}
    </g>
  );
}

function HashRing({ t, L }: { t: number; L: Layout }) {
  const fs = L.fs;
  const r = L.syncer;
  const hashes = ["a3", "7f", "c1", "e9", t >= 66.8 ? "5b≠2d" : "5b"];
  const cw = (L.vertical ? 34 : 29) * fs;
  const lastW = (L.vertical ? 70 : 54) * fs;
  const x0 = r.x + r.w - 12 - (hashes.length - 1) * (cw + 6) - lastW;
  const y = r.y + 10;
  const h = r.h - 20;
  return (
    <g>
      <rect x={x0 - 6} y={r.y + 3} width={r.x + r.w - x0} height={r.h - 6} fill="#fff" />
      {hashes.map((hx, i) => {
        const last = i === hashes.length - 1;
        const w = last ? lastW : cw;
        const x = x0 + i * (cw + 6);
        const bad = last && t >= 66.8;
        return (
          <g key={i}>
            <Box x={x} y={y} w={w} h={h} seed={500 + i} p={seg(t, 66.6 + i * 0.1, 66.9 + i * 0.1)} color={bad ? ORANGE : "#000"} sw={bad ? 2 : 1.3} />
            <T x={x + w / 2} y={y + h / 2 + 4 * fs} s={11.5 * fs} a="middle" w={bad ? 700 : 400}>
              {hx}
            </T>
          </g>
        );
      })}
      {t >= 69.2 && (
        <T x={r.x + r.w - 12} y={r.y - 5} s={17 * fs} a="end" fill={BLUE} hand>
          parent hash changed: roll back, replay
        </T>
      )}
    </g>
  );
}

/* ---------------- chapter 8: on chain, and the math ---------------- */

const ATTRS: [string, string, string][] = [
  ["arkiv.schema", "str", "arkivector.v1.agent.memory"],
  ["av.id", "str", "cat"],
  ["av.vec", "bytes", "41 56 12 00 04 00 …"],
  ["av.doc", "bytes", "Cats purr."],
  ["topic", "str", "pets"],
];

const OPS: [string, string, string][] = [
  ["<->", "L2", "√Σ(uᵢ − vᵢ)²"],
  ["<#>", "neg. inner product", "−Σ uᵢvᵢ"],
  ["<=>", "cosine", "1 − u·v / (‖u‖‖v‖)"],
  ["<+>", "L1", "Σ |uᵢ − vᵢ|"],
  ["<~>", "Hamming", "popcount(u ⊕ v)"],
  ["<%>", "Jaccard", "1 − |u∧v| / |u∨v|"],
];

function Finale({ t, L }: { t: number; L: Layout }) {
  const fs = L.fs;
  const wide = L.vertical;
  const card: R = wide ? { x: 30, y: 40, w: 440, h: 300 } : { x: 18, y: 18, w: 504, h: 330 };
  const ops: R = wide ? { x: 500, y: 40, w: 430, h: 470 } : { x: 18, y: 372, w: 504, h: 470 };
  const pCard = seg(t, 74.4, 75.4);
  const pOps = seg(t, 80.2, 81);
  const s = 12.5 * fs;
  return (
    <svg viewBox={`0 0 ${L.vw} ${L.vh}`} className="block w-full h-auto select-none" role="img" aria-label="An Arkiv entity with its attributes, and pgvector's six distance operators">
      <InkDefs />
      {/* the entity card */}
      <Box x={card.x} y={card.y} w={card.w} h={card.h} seed={700} p={pCard} sw={2} />
      {pCard >= 1 && (
        <>
          <T x={card.x + 16} y={card.y + 30 * fs} s={15 * fs} w={700}>
            Arkiv entity
          </T>
          <T x={card.x + card.w - 16} y={card.y + 30 * fs} s={11.5 * fs} a="end">
            {`owner 0xc41e…5713 · block ${N}`}
          </T>
          {ATTRS.map(([k, ty, v], i) => {
            const at = 75.2 + i * 0.5;
            if (t < at) return null;
            const y = card.y + (64 + i * 44) * fs;
            const hot = (k === "av.vec" || k === "av.doc" || k === "arkiv.schema") && t >= 77 && t < 80.4;
            return (
              <g key={k}>
                <Stroke d={line([card.x + 14, y + 14 * fs], [card.x + card.w - 14, y + 14 * fs], 710 + i)} w={1} dash="2 3" />
                <T x={card.x + 16} y={y} s={s} w={700} fill={hot ? ORANGE : "#000"}>
                  {k}
                </T>
                <T x={card.x + 16 + (wide ? 120 : 142)} y={y} s={s}>
                  {ty}
                </T>
                <T x={card.x + 16 + (wide ? 182 : 212)} y={y} s={s}>
                  {v}
                </T>
              </g>
            );
          })}
        </>
      )}

      {/* the operators */}
      {pOps > 0 && (
        <>
          <Box x={ops.x} y={ops.y} w={ops.w} h={ops.h} seed={720} p={pOps} sw={2} />
          {pOps >= 1 && (
            <T x={ops.x + 16} y={ops.y + 30 * fs} s={15 * fs} w={700}>
              pgvector's distance operators
            </T>
          )}
          {OPS.map(([op, name, f], i) => {
            const at = 81 + i * 0.45;
            if (t < at) return null;
            const y = ops.y + (68 + i * 46) * fs;
            const hot = op === "<=>" && t >= 84.4;
            return (
              <g key={op}>
                {hot && <rect x={ops.x + 12} y={y - 17 * fs} width={ops.w - 24} height={25 * fs} fill={ORANGE} />}
                <T x={ops.x + 16} y={y} s={14 * fs} w={700}>
                  {op}
                </T>
                <T x={ops.x + 16 + 50 * fs} y={y} s={s}>
                  {name}
                </T>
                <T x={ops.x + ops.w - 16} y={y} s={14 * fs} a="end" w={500}>
                  {f}
                </T>
              </g>
            );
          })}
          {wide && t >= 82 && <CosineSketch t={t} x={ops.x + 120} y={ops.y + 420} />}
        </>
      )}
      {wide && t >= 83 && (
        <T x={card.x + 10} y={card.y + card.h + 70} s={26} fill={BLUE} hand>
          any Arkiv client can read this entity.
        </T>
      )}
      {wide && t >= 84.4 && (
        <T x={card.x + 10} y={card.y + card.h + 110} s={26} fill={BLUE} hand>
          Arkivector adds the search.
        </T>
      )}
    </svg>
  );
}

function CosineSketch({ t, x, y }: { t: number; x: number; y: number }) {
  const p = seg(t, 82, 83.2);
  const a1 = -0.25;
  const a2 = -0.85;
  const L = 150;
  const u: P2 = [x + Math.cos(a1) * L, y + Math.sin(a1) * L];
  const v: P2 = [x + Math.cos(a2) * L * 0.8, y + Math.sin(a2) * L * 0.8];
  return (
    <g>
      <Arrow a={[x, y]} b={u} seed={801} p={p} w={2} />
      <Arrow a={[x, y]} b={v} seed={802} p={p} w={2} color={ORANGE} />
      {p >= 1 && (
        <>
          <Stroke d={`M${x + Math.cos(a1) * 46} ${y + Math.sin(a1) * 46} A46 46 0 0 0 ${x + Math.cos(a2) * 46} ${y + Math.sin(a2) * 46}`} w={1.6} stroke={ORANGE} />
          <T x={x + 54} y={y - 24} s={15} w={600}>
            θ
          </T>
          <T x={u[0] + 8} y={u[1] + 4} s={13} w={600}>
            u
          </T>
          <T x={v[0] + 6} y={v[1] - 4} s={13} w={600} fill={ORANGE}>
            v
          </T>
        </>
      )}
    </g>
  );
}
