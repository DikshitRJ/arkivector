import { HeroDemo } from "../anim/HeroDemo";
import { SearchDemo } from "../anim/SearchDemo";
import { Page, REPO } from "../components/Layout";
import { ArkivMark, Button, CodeBlock, H2, Note } from "../components/ui";
import { Link } from "../router";

const FACTS: { n: string; what: string }[] = [
  { n: "97", what: "parity tests against pgvector 0.8.7 running in Postgres. Every distance is bit-identical or within float32 rounding." },
  { n: "4", what: "vector types, same as pgvector: vector, halfvec, sparsevec, bit." },
  { n: "6", what: "distance operators: <-> L2, <#> negative inner product, <=> cosine, <+> L1, <~> Hamming, <%> Jaccard." },
  { n: "< 3 s", what: "from upsert to mined transaction on the Arkiv Rabdi testnet in our end-to-end run." },
  { n: "18 MB", what: "container image. One daemon, written in C11, plus thin Python and Node.js SDKs." },
];

const SAMPLE = `from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)

client.upsert("agent.memory", [
    EntityInput(id="cat", vector=[1, 0, 0, 0], document="Cats purr.", attributes={"topic": "pets"}),
], wait="receipt")      # signed, sent to Arkiv, mined

res = client.search("agent.memory", vector=[1, 0.05, 0, 0], k=2, filter=F.eq("topic", "pets"))
print([(h.id, h.distance) for h in res.hits])`;

/** The main call to action: one big, gently animated button into the install guide. */
function StartButton() {
  return (
    <Link to="/docs/install" className="start-btn group mt-7 no-underline" aria-label="Install and get started">
      <span className="start-btn__sweep" aria-hidden />
      <span className="relative font-display text-[22px] sm:text-[26px] font-bold leading-tight">Install and get started</span>
      <svg className="start-btn__arrow relative shrink-0" width="38" height="24" viewBox="0 0 44 28" aria-hidden>
        <path d="M2 14 H38 M27 3 L40 14 L27 25" fill="none" stroke="#fe7445" strokeWidth="3.2" strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    </Link>
  );
}

/** A pointer for AI agents reading the page: the same content as Markdown files. */
function AgentNote() {
  const files = [
    { href: "/agents.md", label: "/agents.md", hint: "the whole site" },
    { href: "/docs.md", label: "/docs.md", hint: "every docs page, one file" },
    { href: "/llms.txt", label: "/llms.txt", hint: "index" },
  ];
  return (
    <aside
      aria-label="Documentation for AI agents"
      className="mt-5 w-fit max-w-full border-[1.5px] border-dashed border-ink bg-paper px-3.5 py-2.5 text-[13px] leading-relaxed"
    >
      <p className="flex items-center gap-2 font-semibold">
        <svg width="18" height="18" viewBox="0 0 24 24" aria-hidden className="shrink-0">
          <rect x="4" y="7" width="16" height="12" rx="2" fill="none" stroke="#000" strokeWidth="1.8" />
          <path d="M12 3v4M8.5 12.5v1.5M15.5 12.5v1.5M9 16.5h6" stroke="#000" strokeWidth="1.8" strokeLinecap="round" />
          <circle cx="12" cy="3" r="1.4" fill="#fe7445" />
        </svg>
        If you are an AI agent, this documentation is written for you:
      </p>
      <p className="mt-1 flex flex-wrap gap-x-4 gap-y-0.5 pl-[26px]">
        {files.map((f) => (
          <span key={f.href}>
            <a href={f.href} className="ink-link font-semibold">
              {f.label}
            </a>{" "}
            <span className="opacity-70">({f.hint})</span>
          </span>
        ))}
      </p>
    </aside>
  );
}

export function Home() {
  return (
    <>
      {/* hero */}
      <Page className="pt-10 sm:pt-16 pb-8">
        <div className="grid md:grid-cols-[minmax(0,3fr)_minmax(0,1fr)] gap-6 items-end">
          <div>
            <h1 className="text-[40px] sm:text-[56px]">
              <span className="bg-paper">Vector search for</span>
              <br />
              <ArkivMark className="bg-paper" />
            </h1>
            <p className="mt-4 max-w-[60ch] text-[16px] bg-paper">
              Arkivector runs pgvector's distance functions over entities stored on the Arkiv chain. Your embeddings are{" "}
              <span className="marker">written on chain by your wallet</span>, and you query them with one REST call or from Python and Node.js.
            </p>
            <StartButton />
            <p className="mt-4 flex flex-wrap gap-x-5 gap-y-1 text-[13.5px] bg-paper w-fit">
              <Link to="/docs/getting-started/quickstart" className="ink-link">
                quick start
              </Link>
              <Link to="/architecture" className="ink-link">
                watch how it works
              </Link>
              <Link to="/docs/mcp" className="ink-link">
                connect an AI agent
              </Link>
            </p>
            <AgentNote />
          </div>
          <Note className="hidden md:block -rotate-3 pb-2 text-[30px]! lg:text-[38px]! leading-[1.05]!">
            pgvector, except the rows live on a blockchain
          </Note>
        </div>
      </Page>

      <Page className="pb-14">
        <HeroDemo />
      </Page>

      {/* why */}
      <Page className="pb-14">
        <div className="grid md:grid-cols-2 gap-6">
          <section className="sheet px-5 py-4">
            <H2>pgvector made retrieval boring</H2>
            <p>
              pgvector is the Postgres extension that turned "where do I keep my embeddings?" into{" "}
              <code className="chip">ORDER BY embedding {"<=>"} $1 LIMIT 5</code>. It has more than 23,000 GitHub stars and sits
              under a large share of RAG and agent stacks, because it adds vector search to a database people already run.
            </p>
          </section>
          <section className="sheet px-5 py-4">
            <H2>
              <ArkivMark /> had no vector search
            </H2>
            <p>
              Arkiv is a chain built for data: entities with typed attributes, an owner, and an expiry block, queried with a
              filter language. You could store an embedding there as bytes, but you could not ask which entities are closest to
              it. Arkivector adds that, using pgvector's own C code for the math.
            </p>
          </section>
        </div>
      </Page>

      {/* search demo */}
      <Page className="pb-14">
        <H2>Search is geometry</H2>
        <p className="max-w-[70ch] mb-5 bg-paper">
          An embedding model places texts with similar meaning close together. A query becomes a vector too, and search asks
          which stored vectors point in nearly the same direction. Below, the query swings around and the ranking is
          recomputed every frame.
        </p>
        <SearchDemo />
      </Page>

      {/* facts */}
      <Page className="pb-14">
        <H2>Numbers we can back up</H2>
        <div className="sheet">
          {FACTS.map((f, i) => (
            <div
              key={f.n}
              className={`grid grid-cols-[5.5rem_minmax(0,1fr)] sm:grid-cols-[8rem_minmax(0,1fr)] ${i ? "border-t border-dashed border-ink" : ""}`}
            >
              <div className="border-r-[1.5px] border-ink px-3 py-3 font-display text-[22px] sm:text-[26px] font-bold whitespace-nowrap">
                {f.n}
              </div>
              <div className="px-4 py-3 self-center">{f.what}</div>
            </div>
          ))}
        </div>
      </Page>

      {/* code */}
      <Page className="pb-16">
        <div className="grid md:grid-cols-[minmax(0,3fr)_minmax(0,2fr)] gap-6 items-start">
          <div className="min-w-0">
            <H2>Store, then search</H2>
            <CodeBlock code={SAMPLE} lang="python" title="python" />
          </div>
          <div className="md:pt-14">
            <p className="bg-paper">
              The daemon builds the Arkiv transaction, signs it, waits for it to be mined, and indexes the entity once it reads
              it back from the chain. With <code className="chip">session_consistency</code>, the search waits for that block, so
              you always see your own write.
            </p>
            <div className="mt-5 flex flex-wrap gap-4">
              <Button href="/docs/install">Install and get started</Button>
              <Button href={REPO} variant="outline" external>
                Source on GitHub
              </Button>
            </div>
          </div>
        </div>
      </Page>
    </>
  );
}
