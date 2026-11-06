import { HeroDemo } from "../anim/HeroDemo";
import { SearchDemo } from "../anim/SearchDemo";
import { Page, REPO } from "../components/Layout";
import { Button, CodeBlock, H2, Note } from "../components/ui";

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

export function Home() {
  return (
    <>
      {/* hero */}
      <Page className="pt-10 sm:pt-16 pb-8">
        <div className="grid md:grid-cols-[minmax(0,3fr)_minmax(0,1fr)] gap-6 items-end">
          <div>
            <h1 className="text-[40px] sm:text-[56px] max-w-[14ch]">
              <span className="bg-paper">Vector search for Arkiv.</span>
            </h1>
            <p className="mt-4 max-w-[60ch] text-[16px] bg-paper">
              Arkivector runs pgvector's distance functions over entities stored on the Arkiv chain. Your embeddings are
              written on chain by your wallet, and you query them with one REST call or from Python and Node.js.
            </p>
            <div className="mt-6 flex flex-wrap gap-4">
              <Button href="#/install">Install</Button>
              <Button href="#/architecture" variant="outline">
                Watch how it works
              </Button>
            </div>
          </div>
          <Note className="hidden md:block -rotate-3 pb-2">
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
            <H2>Arkiv had no vector search</H2>
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
              <Button href="#/install">Get started</Button>
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
