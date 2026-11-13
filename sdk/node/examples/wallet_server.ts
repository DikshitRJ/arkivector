/**
 * Browser wallet example: entities signed by MetaMask (or any EIP-1193 wallet) in the user's
 * browser, against a daemon that holds NO private key (GitHub issue #12).
 *
 *   npm run build && ARKIVECTOR_URL=http://127.0.0.1:8080 node dist/examples/wallet_server.js
 *   open http://127.0.0.1:3000 in a browser with MetaMask set to the Arkiv network
 *
 * Flow (the backend never sees a key, the browser never needs the SDK):
 *   1. page -> POST /prepare {address, text}  backend: client.prepare(...)  -> unsigned txs as JSON
 *   2. page: ethereum.request({method: "eth_sendTransaction", params: [tx]})  (MetaMask signs + sends)
 *   3. page -> POST /submit {hashes}          backend: client.submit({txHashes, wait: "indexed"})
 *   4. page -> POST /search {text}            backend: client.search(...)
 * The connected address must be a writer of the collection: start the server with
 * WRITERS=0xYourAddress (comma-separated) so it creates the collection with it.
 * In a single-page app you can skip the backend round trips and call
 * client.upsertSigned(collection, entities, { signer: window.ethereum }) directly.
 */

import * as http from "node:http";
import { Arkivector, toRpcTransaction } from "../src/index.js";

const COLLECTION = "wallet.demo";
const DIMS = 8;

/** Toy embedding (hash of character trigrams) so the example needs no model. */
function embed(text: string): number[] {
  const v = new Array<number>(DIMS).fill(0);
  const t = ` ${text.toLowerCase()} `;
  for (let i = 0; i + 3 <= t.length; i++) {
    let h = 0;
    for (const ch of t.slice(i, i + 3)) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
    v[h % DIMS] += 1;
  }
  const n = Math.hypot(...v) || 1;
  return v.map((x) => x / n);
}

const PAGE = `<!doctype html><meta charset="utf-8"><title>Arkivector wallet demo</title>
<style>body{font:15px system-ui;max-width:640px;margin:2rem auto;padding:0 1rem}input{width:70%}pre{background:#f4f4f4;padding:.5rem;white-space:pre-wrap}</style>
<h1>Write with your wallet</h1>
<p><button id="connect">Connect wallet</button> <span id="addr"></span></p>
<p><input id="text" placeholder="A note to store"> <button id="write">Sign &amp; store</button></p>
<p><input id="q" placeholder="Search"> <button id="search">Search</button></p>
<pre id="log"></pre>
<script>
const log = (m) => (document.getElementById("log").textContent += m + "\\n");
const post = async (path, body) => {
  const r = await fetch(path, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
  const j = await r.json(); if (!r.ok) throw new Error(j.error || r.statusText); return j;
};
let address = null;
document.getElementById("connect").onclick = async () => {
  if (!window.ethereum) return log("No EIP-1193 wallet found (install MetaMask).");
  [address] = await window.ethereum.request({ method: "eth_requestAccounts" });
  document.getElementById("addr").textContent = address;
};
document.getElementById("write").onclick = async () => {
  try {
    if (!address) return log("Connect first.");
    const p = await post("/prepare", { address, text: document.getElementById("text").value });
    const want = "0x" + p.chainId.toString(16);
    if ((await window.ethereum.request({ method: "eth_chainId" })) !== want)
      await window.ethereum.request({ method: "wallet_switchEthereumChain", params: [{ chainId: want }] });
    const hashes = [];
    for (const tx of p.transactions) hashes.push(await window.ethereum.request({ method: "eth_sendTransaction", params: [tx] }));
    log("signed and sent: " + hashes.join(", "));
    const s = await post("/submit", { hashes });
    log("indexed at block " + s.blockNumber + ", keys " + s.createdKeys.join(", "));
  } catch (e) { log("error: " + e.message); }
};
document.getElementById("search").onclick = async () => {
  const s = await post("/search", { text: document.getElementById("q").value });
  log(s.hits.map((h) => h.distance.toFixed(3) + "  " + h.document).join("\\n") || "(no results)");
};
</script>`;

async function main(): Promise<void> {
  const url = process.env.ARKIVECTOR_URL ?? "http://127.0.0.1:8080";
  const port = Number(process.env.PORT ?? 3000);
  const writers = (process.env.WRITERS ?? "").split(",").filter(Boolean);
  const client = new Arkivector({ url });
  const v = await client.version();
  console.log(
    `daemon ${v.daemonVersion}; managed signer: ${v.signer ? v.signer.kind : "none (not needed)"}`,
  );
  if (!(await client.hasCapability("entities.submit"))) {
    throw new Error("this daemon predates entities:submit (contract 1.1.0)");
  }
  const existing = (await client.listCollections()).map((c) => c.name);
  if (!existing.includes(COLLECTION)) {
    if (writers.length === 0) {
      throw new Error(
        "set WRITERS=0xYourWalletAddress to create the demo collection",
      );
    }
    await client.createCollection({
      name: COLLECTION,
      column: { type: "vector", dimensions: DIMS },
      index: { method: "none", opclass: "vector_cosine_ops" },
      writers,
      defaultTtl: { days: 7 },
    });
  }

  const read = (req: http.IncomingMessage): Promise<Record<string, unknown>> =>
    new Promise((resolve, reject) => {
      let b = "";
      req.on("data", (c) => (b += c));
      req.on("end", () => {
        try {
          resolve(b ? JSON.parse(b) : {});
        } catch (e) {
          reject(e);
        }
      });
    });

  http
    .createServer(async (req, res) => {
      const send = (status: number, body: unknown) => {
        res.writeHead(status, { "content-type": "application/json" });
        res.end(JSON.stringify(body));
      };
      try {
        if (req.method === "GET") {
          res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
          res.end(PAGE);
        } else if (req.url === "/prepare") {
          const b = await read(req);
          const text = String(b.text ?? "");
          const p = await client.prepare(
            COLLECTION,
            [{ id: `note-${Date.now()}`, vector: embed(text), document: text }],
            { from: String(b.address) },
          );
          // Hex-quantity JSON objects, exactly what eth_sendTransaction expects.
          send(200, {
            chainId: p.chainId,
            transactions: p.transactions.map(toRpcTransaction),
          });
        } else if (req.url === "/submit") {
          const b = await read(req);
          const s = await client.submit(COLLECTION, {
            txHashes: (b.hashes as string[]) ?? [],
            wait: "indexed",
          });
          send(200, { blockNumber: s.blockNumber, createdKeys: s.createdKeys });
        } else if (req.url === "/search") {
          const b = await read(req);
          const r = await client.search(COLLECTION, {
            vector: embed(String(b.text ?? "")),
            k: 5,
            include: ["document"],
          });
          send(200, {
            hits: r.hits.map((h) => ({
              distance: h.distance,
              document: h.document,
            })),
          });
        } else {
          send(404, { error: "not found" });
        }
      } catch (e) {
        // Details go to the server log only; the page gets a generic message.
        console.error(e);
        send(400, { error: "request failed (see the server log)" });
      }
    })
    .listen(port, "127.0.0.1", () =>
      console.log(`open http://127.0.0.1:${port}`),
    );
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
