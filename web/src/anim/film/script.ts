/** Chapters and subtitles for the architecture film. Times are in seconds. */

export type Chapter = { title: string; start: number; end: number };
export type Line = { t: number; text: string };

export const CHAPTERS: Chapter[] = [
  { title: "The parts", start: 0, end: 9 },
  { title: "A write", start: 9, end: 20 },
  { title: "Signed and sent", start: 20, end: 31 },
  { title: "Synced back", start: 31, end: 41 },
  { title: "A search", start: 41, end: 56 },
  { title: "Expiry", start: 56, end: 64 },
  { title: "A reorg", start: 64, end: 74 },
  { title: "On chain, and the math", start: 74, end: 88 },
];

export const TOTAL = CHAPTERS[CHAPTERS.length - 1].end;

/** The block our test upsert landed in on the Rabdi testnet. */
export const N = 306131;

export const LINES: Line[] = [
  { t: 0, text: "Arkivector has three parts." },
  { t: 2, text: "Your code uses a thin SDK, which talks to the arkivectord daemon over REST." },
  { t: 4.6, text: "The daemon reads and writes the Arkiv chain. The chain is the source of truth." },
  { t: 7, text: "Everything in the daemon's index can be rebuilt from Arkiv." },

  { t: 9, text: "An upsert arrives: an id, a vector, a document and some attributes." },
  { t: 11.6, text: "The writer encodes the vector as VectorBytes: the 'AV' magic, a type byte, then pgvector's own struct." },
  { t: 15.2, text: "Everything is wrapped as Arkiv CREATE operations inside one execute((uint8,bytes)[]) call." },

  { t: 20, text: "The daemon signs it with its wallet key: secp256k1, an EIP-1559 transaction." },
  { t: 22.6, text: "It goes to Arkiv with eth_sendRawTransaction." },
  { t: 25, text: `Rabdi makes a block every two seconds. Our upsert lands in block ${N}.` },
  { t: 27.4, text: "The receipt comes back. The entity is on chain, but it is not in the index yet." },

  { t: 31, text: "The syncer follows the chain with eth_getLogs and sees the EntityCreated event." },
  { t: 33.6, text: "It fetches the entity and stores it: the row in LMDB, the vector in a memory-mapped segment." },
  { t: 36, text: "Nothing is indexed until it has been read back from the chain, so the index cannot drift from Arkiv." },
  { t: 38.6, text: `The SDK remembers block ${N}, so its next read waits until the index has reached it.` },

  { t: 41, text: "A search arrives: a query vector, k = 2, and a filter on topic." },
  { t: 43.6, text: "The filter becomes a bitmap with one bit per entity whose topic is pets." },
  { t: 45.6, text: "It is ANDed with the bitmap of entities still alive at this block." },
  { t: 47.6, text: "Only the survivors are scored, with pgvector's cosine function: the same C code Postgres runs." },
  { t: 50.6, text: "A heap keeps the best k. Ties go to the smaller entity key." },
  { t: 52.8, text: "The answer comes back with the block it reflects, as_of_block." },

  { t: 56, text: "Every Arkiv entity has an expiry block, and the daemon tracks it." },
  { t: 58.6, text: `fish expires at block ${N + 4}. The chain keeps moving.` },
  { t: 61, text: "Once the chain passes that block, fish leaves the live bitmap. Nobody had to send a delete." },

  { t: 64, text: "Sometimes the chain replaces its newest block. That is a reorg." },
  { t: 66.6, text: "The syncer keeps a ring of recent block hashes and sees that the parent hash no longer matches." },
  { t: 69.2, text: "It rolls the index back to the last common block, then replays the new branch." },

  { t: 74, text: "On chain, each item is a plain Arkiv entity, readable by any Arkiv client." },
  { t: 77, text: "av.vec holds the vector, av.doc the text, and arkiv.schema names the collection for the syncer." },
  { t: 80.4, text: "The distances are pgvector's six operators, compiled from the pgvector 0.8.7 source." },
  { t: 84.4, text: "Same math as pgvector. The data lives on Arkiv." },
];

export function lineAt(t: number): number {
  let i = 0;
  for (let k = 0; k < LINES.length; k++) if (LINES[k].t <= t) i = k;
  return i;
}

export function chapterAt(t: number): number {
  for (let k = CHAPTERS.length - 1; k >= 0; k--) if (CHAPTERS[k].start <= t) return k;
  return 0;
}
