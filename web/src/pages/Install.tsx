import type { ReactNode } from "react";
import { Page, REPO } from "../components/Layout";
import { Button, CodeBlock, Note, Tabs } from "../components/ui";

/* Commands and code are copied from the repository README (quick start). */

const DOCKER = `docker run -d --name arkivectord \\
  -p 127.0.0.1:8080:8080 \\
  -v arkivector-data:/data \\
  -e ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>" \\
  -e ARKIV_PRIVATE_KEY="<wallet private key>" \\
  ghcr.io/dikshitrj/arkivectord:latest

curl http://127.0.0.1:8080/healthz      # {"status":"ok"}`;

const LINUX = `VERSION=1.0.1
curl -LO https://github.com/DikshitRJ/arkivector/releases/download/v\${VERSION}/arkivectord-\${VERSION}-linux-x86_64.tar.gz
tar xzf arkivectord-\${VERSION}-linux-x86_64.tar.gz
cd arkivectord-\${VERSION}-linux-x86_64

export ARKIVECTOR_ARKIV_ACCESS_KEY="<your Arkiv Hub access key>"
export ARKIV_PRIVATE_KEY="<wallet private key>"
export ARKIVECTOR_STORAGE_DATA_DIR="$HOME/.local/share/arkivector"

./bin/arkivectord --config arkivector.example.toml`;

const PIP = `pip install arkivector`;
const NPM = `echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
npm install @dikshitrj/arkivector`;

const PY = `from arkivector import Client, EntityInput, F

client = Client(session_consistency=True)   # reads ARKIVECTOR_URL; waits for your own writes

client.create_collection(
    "agent.memory",
    ("halfvec", 4),                           # use your embedding model's dimension, e.g. 768
    {"method": "hnsw", "opclass": "halfvec_cosine_ops"},
    writers=["0xYourDaemonWalletAddress"],
    attributes={"topic": "str"},
    default_ttl={"days": 7},                  # Arkiv entities expire; this is the default lifetime
)

client.upsert("agent.memory", [
    EntityInput(id="cat", vector=[1, 0, 0, 0], document="Cats purr.", attributes={"topic": "pets"}),
    EntityInput(id="dog", vector=[0.9, 0.1, 0, 0], document="Dogs bark.", attributes={"topic": "pets"}),
    EntityInput(id="car", vector=[0, 0, 1, 0], document="Cars need fuel.", attributes={"topic": "vehicles"}),
], wait="receipt")                            # returns once the transaction is mined

res = client.search("agent.memory", vector=[1, 0.05, 0, 0], k=2, filter=F.eq("topic", "pets"))
for hit in res.hits:
    print(hit.id, round(hit.distance, 4))     # cat 0.0012, dog 0.0018`;

const JS = `import { Arkivector } from "@dikshitrj/arkivector";

const client = new Arkivector({ url: "http://127.0.0.1:8080", sessionConsistency: true });

await client.upsert("agent.memory", [
  { id: "fish", vector: [0.8, 0, 0.2, 0], document: "Fish swim.", attributes: { topic: "pets" } },
]);

const res = await client.search("agent.memory", { vector: [1, 0.05, 0, 0], k: 3 });
console.log(res.hits.map((h) => [h.id, h.distance]));`;

const CURL = `curl -s http://127.0.0.1:8080/v1/collections/agent.memory/search \\
  -H 'content-type: application/json' \\
  -d '{"vector":[1,0.05,0,0],"k":2,"filter":{"eq":{"topic":"pets"}}}'`;

const ENV: { name: string; need: string; what: string }[] = [
  {
    name: "ARKIV_PRIVATE_KEY",
    need: "for writes",
    what: "Hex private key that signs transactions (0x prefix optional). Without it the daemon is read-only. Keep it out of config files and shell history.",
  },
  { name: "ARKIVECTOR_ARKIV_ACCESS_KEY", need: "recommended", what: "Your Arkiv Hub access key, so you are not on the throttled public pool." },
  { name: "ARKIVECTOR_STORAGE_DATA_DIR", need: "binary only", what: "Where the local index lives. The Docker image uses /data." },
  { name: "ARKIVECTOR_URL", need: "SDKs", what: "Where the SDKs find the daemon. Defaults to http://127.0.0.1:8080." },
];

function Step({ n, title, children, last = false }: { n: number; title: string; children: ReactNode; last?: boolean }) {
  return (
    <section className="grid grid-cols-[2.5rem_minmax(0,1fr)] sm:grid-cols-[3.5rem_minmax(0,1fr)] gap-x-3 sm:gap-x-5">
      <div className="relative flex justify-center">
        {!last && <div aria-hidden className="absolute top-12 bottom-0 border-l-2 border-dashed border-arkiv-blue" />}
        <div className="relative z-10 mt-1 h-10 w-10 sm:h-11 sm:w-11 grid place-items-center bg-ink text-paper font-display text-[20px] font-bold shadow-[4px_4px_0_#fe7445]">
          {n}
        </div>
      </div>
      <div className={`min-w-0 ${last ? "" : "pb-12"}`}>
        <h2 className="text-[26px] sm:text-[30px] mt-1 mb-3">
          <span className="bg-paper">{title}</span>
        </h2>
        {children}
      </div>
    </section>
  );
}

const P = ({ children }: { children: ReactNode }) => <p className="my-3 max-w-[72ch] bg-paper">{children}</p>;
const C = ({ children }: { children: ReactNode }) => <code className="chip">{children}</code>;

export function Install() {
  return (
    <Page className="py-10 sm:py-14">
      <h1 className="text-[38px] sm:text-[50px]">
        <span className="bg-paper">Install</span>
      </h1>
      <p className="mt-3 mb-10 max-w-[64ch] text-[16px] bg-paper">
        You need <span className="marker">three things</span>: the daemon (<C>arkivectord</C>), a funded Arkiv wallet if you want to write, and an SDK. Reading
        works without a wallet.
      </p>

      <Step n={1} title="Run the daemon">
        <Tabs
          tabs={[
            {
              id: "docker",
              label: "Docker (recommended)",
              body: (
                <>
                  <P>Pull the image from GitHub Container Registry and run it in the background:</P>
                  <CodeBlock code={DOCKER} lang="bash" title="shell" />
                  <P>
                    The daemon now listens on <C>http://127.0.0.1:8080</C> and syncs from the Arkiv Rabdi testnet. Your index lives in
                    the <C>arkivector-data</C> volume and survives restarts.
                  </P>
                </>
              ),
            },
            {
              id: "linux",
              label: "Linux x86_64 binary",
              body: (
                <>
                  <P>Download the latest release and run it:</P>
                  <CodeBlock code={LINUX} lang="bash" title="shell" />
                  <P>
                    libcurl is bundled in the tarball. You need libuv and OpenSSL 3 from your distribution (<C>apt install libuv1 libssl3</C>,{" "}
                    <C>dnf install libuv openssl</C>, and so on).
                  </P>
                  <P>
                    The daemon needs libcurl 8.11 or newer for WebSocket support. If your distribution ships something older,{" "}
                    <C>daemon/tools/ci-install-curl.sh</C> builds a current one.
                  </P>
                </>
              ),
            },
          ]}
        />
      </Step>

      <Step n={2} title="Give it a wallet">
        <P>Writing means sending Arkiv transactions, and those need a signer and a bit of gas.</P>
        <ol className="my-4 space-y-2 list-decimal pl-6 max-w-[72ch] bg-paper">
          <li>
            Create a fresh EVM wallet for the daemon (MetaMask, Rabby, or anything that can export a private key). Don't reuse the
            wallet that holds your savings.
          </li>
          <li>
            Get free test GLM from the{" "}
            <a className="ink-link" href="https://hub.arkiv.network/faucet" target="_blank" rel="noreferrer">
              Arkiv faucet
            </a>
            .
          </li>
          <li>
            Create an access key at{" "}
            <a className="ink-link" href="https://hub.arkiv.network/access-keys" target="_blank" rel="noreferrer">
              hub.arkiv.network/access-keys
            </a>{" "}
            (sign in with the wallet and pick Rabdi). Without one you share Arkiv's public pool, which is throttled hard.
          </li>
          <li>Pass both to the daemon as environment variables:</li>
        </ol>

        <div className="sheet my-4 overflow-x-auto">
          <table className="w-full min-w-[34rem] border-collapse text-[13.5px]">
            <thead>
              <tr className="border-b-[1.5px] border-ink text-left">
                <th className="px-3 py-2 font-semibold">variable</th>
                <th className="px-3 py-2 font-semibold">needed</th>
                <th className="px-3 py-2 font-semibold">what it does</th>
              </tr>
            </thead>
            <tbody>
              {ENV.map((e) => (
                <tr key={e.name} className="border-b border-dashed border-ink last:border-b-0 align-top">
                  <td className="px-3 py-2 font-semibold whitespace-nowrap text-arkiv-blue">{e.name}</td>
                  <td className="px-3 py-2 whitespace-nowrap">
                    <span className={e.need === "for writes" ? "marker" : ""}>{e.need}</span>
                  </td>
                  <td className="px-3 py-2">{e.what}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>

        <div className="my-6 grid md:grid-cols-[minmax(0,1fr)_14rem] gap-3 items-center">
          <div className="hatch p-1.5 border-[1.5px] border-arkiv-orange shadow-[3px_3px_0_#fe7445]">
            <p className="bg-paper px-3 py-2">
              Put the daemon wallet's address in each collection's <C>writers</C> list. A collection only indexes entities created by
              its writers, so without it your writes land on chain, never show up in search, and you lose an afternoon finding out why.
            </p>
          </div>
          <Note className="md:-rotate-2">this one has bitten us. twice.</Note>
        </div>
      </Step>

      <Step n={3} title="Install an SDK">
        <P>Python and Node.js are supported today. More languages are planned.</P>
        <Tabs
          tabs={[
            { id: "py", label: "Python", body: <CodeBlock code={PIP} lang="bash" title="shell" /> },
            {
              id: "node",
              label: "Node.js",
              body: (
                <>
                  <CodeBlock code={NPM} lang="bash" title="shell" />
                  <P>
                    GitHub Packages asks for a GitHub token with <C>read:packages</C> in your <C>~/.npmrc</C>, even for public packages.
                  </P>
                </>
              ),
            },
          ]}
        />
      </Step>

      <Step n={4} title="Store and search" last>
        <Tabs
          tabs={[
            { id: "py", label: "Python", body: <CodeBlock code={PY} lang="python" title="python" /> },
            { id: "node", label: "Node.js", body: <CodeBlock code={JS} lang="js" title="javascript" /> },
            { id: "http", label: "curl", body: <CodeBlock code={CURL} lang="bash" title="shell" /> },
          ]}
        />
        <P>
          This is the flow we ran against the Rabdi testnet: the upsert transaction landed in under three seconds, and the search
          returned <C>cat</C> and <C>dog</C> with those distances a few blocks later.
        </P>
        <div className="mt-6 flex flex-wrap gap-4">
          <Button href="/architecture">See what happened inside</Button>
          <Button href={`${REPO}#configuration-reference`} variant="outline" external>
            All configuration options
          </Button>
        </div>
      </Step>
    </Page>
  );
}
