import { useRef } from "react";
import { useLoop } from "../anim/time";
import { Page, REPO } from "../components/Layout";
import { Button, H2, Note } from "../components/ui";
import { BLUE, ORANGE } from "../ink/colors";
import { Arrow, Box, Stroke } from "../ink/Ink";
import { line, loop } from "../ink/paths";

const ITEMS: { title: string; today: string; plan: string; issue: number }[] = [
  {
    title: "The daemon signs with a key in an environment variable",
    issue: 12,
    today: "The daemon holds a hot wallet (ARKIV_PRIVATE_KEY).",
    plan: "entities:prepare already returns unsigned transactions keyed to the wallet's real nonces. Next, the SDKs hand them to the user's own wallet (MetaMask, WalletConnect, hardware wallets), plus encrypted keystores and remote signers for servers.",
  },
  {
    title: "Tested on the Rabdi testnet only",
    issue: 16,
    today: "Mainnet is configurable (rpc_urls, chain_id) but untried.",
    plan: "Run the sync and write suites against mainnet once its endpoints are published, and measure gas reservations against real usage there.",
  },
  {
    title: "Linux and Docker only",
    issue: 17,
    today: "macOS and Windows use the container.",
    plan: "Port the few glibc-specific calls.",
  },
  {
    title: "Two SDK languages",
    issue: 18,
    today: "SDKs exist for Python and Node.js.",
    plan: "The repository already has placeholders for C, C++ and Rust SDKs.",
  },
];

/** Shipped after v1.1.0, kept here so the roadmap shows what changed. */
const DONE: { title: string; what: string; issue: number }[] = [
  {
    title: "HNSW graphs survive restarts",
    issue: 11,
    what: "Graphs are saved per expiry band under data_dir/segments and loaded at startup, so only what changed since the save is indexed. Rebuilds run on a background thread while sync keeps going, and a band whose entities have all expired is dropped whole. Corrupt files are rejected and rebuilt, never trusted.",
  },
  {
    title: "Entity keys follow the wallet's on-chain nonce",
    issue: 13,
    what: "Writes read entityNonce from the Arkiv contract before predicting keys, and take the final keys from the EntityCreated receipt logs when they wait for receipts. entities:prepare uses the wallet's real nonces too.",
  },
  {
    title: "Dynamic gas fees",
    issue: 14,
    what: "Fees come from eth_feeHistory (then eth_gasPrice, then config) with a configurable cap, the gas limit is the estimate plus a configurable margin, and a stuck transaction is resent with bumped fees.",
  },
  {
    title: "Qdrant writes are real",
    issue: 15,
    what: "Qdrant upserts and deletes go through the same writer as the REST API, searches return payloads, and a daemon without a signer refuses writes instead of faking success.",
  },
];

/** User-wallet signing: prepare, sign in the browser wallet, send. */
function WalletFlow() {
  const ref = useRef<HTMLDivElement>(null);
  const ms = useLoop(ref, 7000, 6500);
  const k = ms / 7000;
  const steps = [
    { x: 10, t: "your app", s: "SDK" },
    { x: 200, t: "arkivectord", s: "entities:prepare" },
    { x: 390, t: "user's wallet", s: "signs in browser" },
    { x: 580, t: "[ ARKIV ]", s: "mined in a block" },
  ];
  const seg = (i: number) => Math.min(1, Math.max(0, k * 3 - i));
  return (
    <div ref={ref} className="sheet overflow-x-auto">
      <svg viewBox="0 0 760 150" className="block w-full min-w-[34rem] h-auto" role="img" aria-label="Planned flow: the daemon prepares an unsigned transaction, the user's wallet signs it, and it is sent to Arkiv">
        {steps.map((s, i) => (
          <g key={s.t}>
            <Box x={s.x} y={40} w={170} h={64} seed={900 + i} color={i === 3 ? BLUE : i === 2 ? ORANGE : "#000"} sw={i === 2 ? 2.2 : 1.6} />
            <text x={s.x + 12} y={66} fontSize={14} fontWeight={700} fill={i === 3 ? BLUE : "#000"} style={{ fontFamily: i === 3 ? "var(--font-display)" : "var(--font-mono)" }}>
              {s.t}
            </text>
            <text x={s.x + 12} y={86} fontSize={11.5} style={{ fontFamily: "var(--font-mono)" }}>
              {s.s}
            </text>
            {i < 3 && <Arrow a={[s.x + 172, 72]} b={[s.x + 198, 72]} seed={910 + i} p={seg(i)} w={1.8} color={i === 1 ? ORANGE : "#000"} />}
          </g>
        ))}
        <text x={300} y={28} fontSize={19} fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
          unsigned tx
        </text>
        <text x={490} y={128} fontSize={19} fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
          signed tx
        </text>
      </svg>
    </div>
  );
}

/** A three-layer HNSW sketch: greedy descent from the entry point to the nearest node. */
function HnswSketch() {
  const ref = useRef<HTMLDivElement>(null);
  const ms = useLoop(ref, 8000, 7600);
  const p = ms / 8000;
  const layers = [
    { y: 34, nodes: [[90, 0], [430, 0]] },
    { y: 96, nodes: [[90, 0], [240, 0], [430, 0], [560, 0]] },
    { y: 158, nodes: [[60, 0], [140, 0], [240, 0], [330, 0], [430, 0], [500, 0], [560, 0], [640, 0]] },
  ];
  const path: [number, number][] = [
    [90, 34],
    [430, 34],
    [430, 96],
    [560, 96],
    [560, 158],
    [500, 158],
  ];
  const n = Math.min(path.length - 1, Math.floor(p * 1.4 * (path.length - 1)));
  return (
    <div ref={ref} className="sheet overflow-x-auto">
      <svg viewBox="0 0 720 200" className="block w-full min-w-[34rem] h-auto" role="img" aria-label="HNSW: a layered graph searched greedily from the top layer down">
        {layers.map((l, li) => (
          <g key={li}>
            <Stroke d={line([30, l.y], [690, l.y], 950 + li)} w={0.8} dash="1 5" />
            <text x={692} y={l.y + 4} fontSize={12} textAnchor="end" style={{ fontFamily: "var(--font-mono)" }}>
              {`layer ${2 - li}`}
            </text>
            {l.nodes.slice(0, -1).map((nd, i) => (
              <Stroke key={i} d={line([nd[0], l.y], [l.nodes[i + 1][0], l.y], 960 + li * 10 + i)} w={1.1} />
            ))}
            {l.nodes.map((nd) => (
              <circle key={nd[0]} cx={nd[0]} cy={l.y} r={6} fill="#fff" stroke="#000" strokeWidth={1.5} />
            ))}
          </g>
        ))}
        {path.slice(0, n + 1).map((pt, i) =>
          i ? <Arrow key={i} a={path[i - 1]} b={pt} seed={980 + i} w={2.2} color={ORANGE} /> : null,
        )}
        <circle cx={path[n][0]} cy={path[n][1]} r={7} fill={ORANGE} stroke="#000" strokeWidth={1.5} />
        {n === path.length - 1 && <Stroke d={loop(500, 158, 16, 14, 990)} w={1.8} stroke={ORANGE} />}
        <text x={40} y={22} fontSize={18} fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
          entry point
        </text>
        <text x={516} y={192} fontSize={18} fill={BLUE} style={{ fontFamily: "var(--font-hand)" }}>
          nearest, after visiting 6 of 14 nodes
        </text>
      </svg>
    </div>
  );
}

export function Next() {
  return (
    <>
      <Page className="pt-10 sm:pt-14 pb-6">
        <h1 className="text-[38px] sm:text-[50px]">
          <span className="bg-paper">What's next</span>
        </h1>
        <p className="mt-3 max-w-[64ch] text-[16px] bg-paper">
          Everything in the install guide works and was tested end to end, including on the Rabdi testnet. These are the parts that
          are not finished yet, and how each one gets fixed. Below them: what was fixed since v1.1.0.
        </p>
      </Page>

      <Page className="pb-12">
        <div className="sheet">
          <div className="hidden md:grid grid-cols-[minmax(0,1.1fr)_minmax(0,1fr)_minmax(0,1.2fr)] border-b-[1.5px] border-ink text-[13px] font-semibold">
            <div className="px-4 py-2">limitation</div>
            <div className="px-4 py-2 border-l border-dashed border-ink">today</div>
            <div className="px-4 py-2 border-l border-dashed border-ink">plan</div>
          </div>
          {ITEMS.map((it, i) => (
            <div
              key={it.title}
              className={`grid md:grid-cols-[minmax(0,1.1fr)_minmax(0,1fr)_minmax(0,1.2fr)] ${i ? "border-t border-dashed border-ink" : ""}`}
            >
              <div className="px-4 pt-3 md:py-3 font-semibold">
                {it.title}
                <a href={`${REPO}/issues/${it.issue}`} target="_blank" rel="noreferrer" className="ml-2 font-mono text-[12.5px] font-normal">
                  #{it.issue}
                </a>
              </div>
              <div className="px-4 py-1 md:py-3 md:border-l border-dashed border-ink text-[13.5px]">
                <span className="md:hidden font-semibold">Today: </span>
                {it.today}
              </div>
              <div className="px-4 pb-3 md:py-3 md:border-l border-dashed border-ink text-[13.5px]">
                <span className="md:hidden font-semibold">Plan: </span>
                <span className="border-l-4 border-arkiv-orange pl-2 block">{it.plan}</span>
              </div>
            </div>
          ))}
        </div>
      </Page>

      <Page className="pb-12">
        <H2>Done since v1.1.0</H2>
        <div className="sheet">
          {DONE.map((it, i) => (
            <div
              key={it.title}
              className={`grid md:grid-cols-[minmax(0,1fr)_minmax(0,2.3fr)] ${i ? "border-t border-dashed border-ink" : ""}`}
            >
              <div className="px-4 pt-3 md:py-3 font-semibold">
                <span className="text-arkiv-blue mr-1" aria-hidden="true">
                  ✓
                </span>
                {it.title}
                <a href={`${REPO}/issues/${it.issue}`} target="_blank" rel="noreferrer" className="ml-2 font-mono text-[12.5px] font-normal">
                  #{it.issue}
                </a>
              </div>
              <div className="px-4 pb-3 md:py-3 md:border-l border-dashed border-ink text-[13.5px]">{it.what}</div>
            </div>
          ))}
        </div>
      </Page>

      <Page className="pb-12">
        <H2>Signing with the user's wallet</H2>
        <p className="mb-4 max-w-[70ch] bg-paper">
          The daemon already builds unsigned transactions through <code className="chip">entities:prepare</code>. The missing piece is
          handing them to a browser or hardware wallet, so the daemon never holds the key.
        </p>
        <WalletFlow />
      </Page>

      <Page className="pb-16">
        <H2>How HNSW search works</H2>
        <p className="mb-4 max-w-[70ch] bg-paper">
          Collections with an hnsw index keep pgvector's graph next to the exact vectors. A search starts at the top layer, walks
          greedily towards the query, then drops a layer and repeats, so it scores a small fraction of the vectors. The planner picks
          the graph for large candidate sets and an exact scan for small or very selective ones, and every hit is rescored with the
          real operator. Graphs are kept per expiry band, saved to disk and rebuilt in the background.
        </p>
        <HnswSketch />
        <div className="mt-8 flex flex-wrap items-center gap-4">
          <Button href={`${REPO}/issues/19`} external>
            Follow the roadmap on GitHub
          </Button>
          <Note className="-rotate-1">pull requests welcome. so are bug reports.</Note>
        </div>
      </Page>
    </>
  );
}
