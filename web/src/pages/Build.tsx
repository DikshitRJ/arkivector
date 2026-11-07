import type { ReactNode } from "react";
import { ArchDiagram } from "../anim/ArchDiagram";
import { Page, REPO } from "../components/Layout";
import { CodeBlock, H2, Note } from "../components/ui";

const V = "1.0.1";

type Artifact = { n: number; what: string; where: { label: string; url: string }[]; cmd: string; lang: string; note: ReactNode };

const ARTIFACTS: Artifact[] = [
  {
    n: 1,
    what: "Python SDK",
    where: [{ label: "pypi.org/project/arkivector", url: "https://pypi.org/project/arkivector/" }],
    cmd: `pip install arkivector==${V}`,
    lang: "shell",
    note: "Pure Python, no dependencies. Wheel and sdist are also attached to every GitHub release.",
  },
  {
    n: 2,
    what: "Node.js SDK",
    where: [{ label: "GitHub Packages · @dikshitrj/arkivector", url: `${REPO}/pkgs/npm/arkivector` }],
    cmd: `echo "@dikshitrj:registry=https://npm.pkg.github.com" >> .npmrc
npm install @dikshitrj/arkivector@${V}`,
    lang: "shell",
    note: "ESM, TypeScript types, zero runtime dependencies. GitHub Packages asks for a token with read:packages.",
  },
  {
    n: 3,
    what: "Daemon, container image",
    where: [{ label: "ghcr.io/dikshitrj/arkivectord", url: `${REPO}/pkgs/container/arkivectord` }],
    cmd: `docker pull ghcr.io/dikshitrj/arkivectord:${V}   # also :1 and :latest
docker run -d -p 127.0.0.1:8080:8080 -v arkivector-data:/data \\
  -e ARKIVECTOR_ARKIV_ACCESS_KEY=... -e ARKIV_PRIVATE_KEY=... \\
  ghcr.io/dikshitrj/arkivectord:${V}`,
    lang: "shell",
    note: "18 MB Alpine image, linux/amd64, runs as a non-root user, index in the /data volume.",
  },
  {
    n: 4,
    what: "Daemon, Linux binary",
    where: [{ label: `GitHub Releases · v${V}`, url: `${REPO}/releases/tag/v${V}` }],
    cmd: `curl -LO ${REPO}/releases/download/v${V}/arkivectord-${V}-linux-x86_64.tar.gz
curl -LO ${REPO}/releases/download/v${V}/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
tar xzf arkivectord-${V}-linux-x86_64.tar.gz`,
    lang: "shell",
    note: "x86_64 glibc build with libcurl bundled; needs libuv and OpenSSL 3 from the OS.",
  },
  {
    n: 5,
    what: "Arkiv Rabdi testnet",
    where: [
      { label: "rpc.rabdi.db-chain.testnet.arkiv.network", url: "https://rpc.rabdi.db-chain.testnet.arkiv.network" },
      { label: "explorer", url: "https://rabdi.explorer.arkiv.network" },
      { label: "faucet", url: "https://hub.arkiv.network/faucet" },
    ],
    cmd: `curl -s https://rpc.rabdi.db-chain.testnet.arkiv.network \\
  -H 'content-type: application/json' \\
  -d '{"jsonrpc":"2.0","id":1,"method":"eth_chainId","params":[]}'   # 0x76edaf = 7794095`,
    lang: "shell",
    note: "Not something we ship: the chain the daemon reads from and writes to. Chain id 7794095, 2 s blocks.",
  },
];

const PIPE = [
  { t: "git tag v1.0.1", s: "push the tag" },
  { t: "release.yml", s: "GitHub Actions" },
  { t: "build", s: "binary · image · wheel · npm" },
  { t: "smoke test", s: "binary and image report the version" },
  { t: "publish", s: "Release · GHCR · PyPI · GitHub npm" },
];

export function Build() {
  return (
    <>
      <Page className="pt-10 sm:pt-14 pb-6">
        <h1 className="text-[38px] sm:text-[50px]">
          <span className="bg-paper">Build artifacts</span>
        </h1>
        <p className="mt-3 max-w-[64ch] text-[16px] bg-paper">
          Where every piece is published, and how to get it. The numbers on the diagram match the list below.
        </p>
      </Page>

      <Page className="pb-10">
        <ArchDiagram markers />
      </Page>

      <Page className="pb-12">
        <H2>How a release is made</H2>
        <ol className="flex flex-col md:flex-row md:items-stretch gap-2 md:gap-0">
          {PIPE.map((p, i) => (
            <li key={p.t} className="flex flex-col md:flex-row md:flex-1 items-stretch">
              <div className={`sheet flex-1 px-3 py-2 ${i === PIPE.length - 1 ? "border-arkiv-orange border-2 shadow-[3px_3px_0_#fe7445]" : ""}`}>
                <p className="font-semibold text-[13.5px] whitespace-nowrap">{p.t}</p>
                <p className="text-[12.5px]">{p.s}</p>
              </div>
              {i < PIPE.length - 1 && (
                <span aria-hidden className="self-center px-1 md:px-2 text-[18px] leading-none">
                  <span className="hidden md:inline">→</span>
                  <span className="md:hidden">↓</span>
                </span>
              )}
            </li>
          ))}
        </ol>
        <p className="mt-3 text-[13px] max-w-[72ch] bg-paper">
          One workflow, <code className="chip">.github/workflows/release.yml</code>, runs on every <code className="chip">v*</code> tag. PyPI
          uses trusted publishing, so there is no token in the repository.
        </p>
      </Page>

      <Page className="pb-16">
        <H2>Artifacts</H2>
        <div className="space-y-8">
          {ARTIFACTS.map((a) => (
            <section key={a.n} className="grid grid-cols-[2.5rem_minmax(0,1fr)] gap-x-3">
              <div className="mt-1 h-9 w-9 grid place-items-center rounded-full bg-arkiv-orange border-[1.5px] border-ink font-bold">{a.n}</div>
              <div className="min-w-0">
                <h3 className="text-[21px]">
                  <span className="bg-paper">{a.what}</span>
                </h3>
                <p className="mt-1 flex flex-wrap gap-x-4 gap-y-1 text-[13.5px]">
                  {a.where.map((w) => (
                    <a key={w.url} className="ink-link break-all" href={w.url} target="_blank" rel="noreferrer">
                      {w.label}
                    </a>
                  ))}
                </p>
                <CodeBlock code={a.cmd} lang={a.lang} title={a.lang} />
                <p className="text-[13.5px] max-w-[72ch] bg-paper">{a.note}</p>
              </div>
            </section>
          ))}
        </div>
        <Note className="mt-10 -rotate-1">every artifact is MIT licensed, and the copied pgvector code keeps its PostgreSQL License.</Note>
      </Page>
    </>
  );
}
