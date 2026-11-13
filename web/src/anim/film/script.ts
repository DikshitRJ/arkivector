/**
 * Chapters and subtitles for the architecture film. Times are in seconds.
 * `say` is what the narrator reads when the written line would sound wrong out loud.
 */

export type Chapter = { title: string; start: number; end: number; summary: string; still: number };
export type Line = { t: number; text: string; say?: string };

/** The block our test upsert landed in on the Rabdi testnet. */
export const N = 306131;

export const CHAPTERS: Chapter[] = [
  { title: "The parts", start: 0, end: 9, still: 8, summary: "SDK, daemon, chain. The chain is the source of truth." },
  { title: "A write", start: 9, end: 20, still: 14.6, summary: "The vector becomes VectorBytes inside an Arkiv CREATE operation." },
  { title: "Signed and sent", start: 20, end: 31, still: 26.5, summary: `Signed with secp256k1, mined in block ${N}.` },
  { title: "Synced back", start: 31, end: 41, still: 36, summary: "The syncer reads the entity back from the chain, then indexes it." },
  { title: "A search", start: 41, end: 56, still: 52, summary: "Filter bitmap, AND live bitmap, score survivors, keep the best k." },
  { title: "The arithmetic", start: 56, end: 74, still: 72, summary: "Cosine distance worked out by hand for cat and dog." },
  { title: "16 bits per number", start: 74, end: 86, still: 85, summary: "How halfvec stores 0.1 as IEEE binary16." },
  { title: "Exact scan and HNSW", start: 86, end: 100, still: 99, summary: "Small searches scan exactly with a top-k heap; large ones walk pgvector's HNSW graph." },
  { title: "Expiry", start: 100, end: 108, still: 106.5, summary: "When the chain passes an entity's expiry block, it leaves the index." },
  { title: "A reorg", start: 108, end: 118, still: 116, summary: "Mismatched block hash, roll back, replay the new branch." },
  { title: "On chain, and the math", start: 118, end: 132, still: 131, summary: "A plain Arkiv entity, and pgvector's six operators." },
];

export const TOTAL = CHAPTERS[CHAPTERS.length - 1].end;

export const LINES: Line[] = [
  { t: 0, text: "Arkivector has three parts." },
  { t: 2, text: "Your code uses a thin SDK, which talks to the arkivectord daemon over REST.", say: "Your code uses a thin SDK, which talks to the Arkivector daemon over REST." },
  { t: 4.6, text: "The daemon reads and writes the Arkiv chain. The chain is the source of truth." },
  { t: 7, text: "Everything in the daemon's index can be rebuilt from Arkiv." },

  { t: 9, text: "An upsert arrives: an id, a vector, a document and some attributes." },
  { t: 11.6, text: "The writer encodes the vector as VectorBytes: the 'AV' magic, a type byte, then pgvector's own struct.", say: "The writer encodes the vector as Vector Bytes: two magic letters, A V, a type byte, then pgvector's own struct." },
  { t: 15.2, text: "Everything is wrapped as Arkiv CREATE operations inside one execute((uint8,bytes)[]) call.", say: "Everything is wrapped as Arkiv create operations inside a single execute call." },

  { t: 20, text: "The daemon signs it with its wallet key: secp256k1, an EIP-1559 transaction.", say: "The daemon signs it with its wallet key, as an E I P 1559 transaction." },
  { t: 22.6, text: "It goes to Arkiv with eth_sendRawTransaction.", say: "It goes to Arkiv as a raw transaction." },
  { t: 25, text: `Rabdi makes a block every two seconds. Our upsert lands in block ${N}.`, say: "The Rabdi testnet makes a block every two seconds. Our upsert lands in the next one." },
  { t: 27.4, text: "The receipt comes back. The entity is on chain, but it is not in the index yet." },

  { t: 31, text: "The syncer follows the chain with eth_getLogs and sees the EntityCreated event.", say: "The syncer follows the chain's logs and sees the entity created event." },
  { t: 33.6, text: "It fetches the entity and stores it: the row in LMDB, the vector in a memory-mapped segment.", say: "It fetches the entity and stores it: the row in L M D B, the vector in a memory mapped segment." },
  { t: 36, text: "Nothing is indexed until it has been read back from the chain, so the index cannot drift from Arkiv." },
  { t: 38.6, text: `The SDK remembers block ${N}, so its next read waits until the index has reached it.`, say: "The SDK remembers that block, so its next read waits until the index has reached it." },

  { t: 41, text: "A search arrives: a query vector, k = 2, and a filter on topic.", say: "A search arrives: a query vector, k equals two, and a filter on topic." },
  { t: 43.6, text: "The filter becomes a bitmap with one bit per entity whose topic is pets." },
  { t: 45.6, text: "It is ANDed with the bitmap of entities still alive at this block.", say: "It is combined, with a bitwise and, with the bitmap of entities still alive at this block." },
  { t: 47.6, text: "Only the survivors are scored, with pgvector's cosine function: the same C code Postgres runs." },
  { t: 50.6, text: "A heap keeps the best k. Ties go to the smaller entity key." },
  { t: 52.8, text: "The answer comes back with the block it reflects, as_of_block.", say: "The answer comes back with the block it reflects." },

  { t: 56, text: "Here is the arithmetic for that search. The query is q = [1, 0.05, 0, 0].", say: "Here is the arithmetic for that search. The query is one, zero point zero five, zero, zero." },
  { t: 59.4, text: "Cosine distance: one minus the dot product, divided by both lengths." },
  { t: 62.8, text: "cat = [1, 0, 0, 0]: dot product 1, lengths 1 and 1.00125, distance 0.00125.", say: "For cat, the dot product is one, the lengths are one and one point zero zero one two five, so the distance is zero point zero zero one two five." },
  { t: 67.2, text: "dog = [0.9, 0.1, 0, 0]: 0.905 / (0.9055 × 1.00125), distance 0.00184. Cat first, dog second.", say: "For dog, zero point nine zero five over zero point nine zero five five times one point zero zero one two five gives zero point zero zero one eight four. Cat first, dog second." },
  { t: 71, text: "Euclidean distance agrees here: 0.05 for cat, 0.112 for dog.", say: "Euclidean distance agrees here: zero point zero five for cat, zero point one one two for dog." },

  { t: 74, text: "The collection stores halfvec: 16 bits per number instead of 32.", say: "The collection stores half vectors: sixteen bits per number instead of thirty two." },
  { t: 77.4, text: "0.1 becomes sign 0, exponent 01011, mantissa 1001100110: exactly 0.0999755859375.", say: "Zero point one becomes a sign bit, five exponent bits and ten mantissa bits, which is exactly zero point zero nine nine nine seven five five eight five nine three seven five." },
  { t: 81.6, text: "The conversion rounds half to even with pgvector's own code, so the stored bytes match Postgres." },

  { t: 86, text: "Small or very selective searches are an exact scan: score each candidate, keep the best k in a heap." },
  { t: 89.4, text: "That is one distance per candidate, O(n log k) in total, and it never misses a neighbour.", say: "That is one distance per candidate, n log k in total, and it never misses a neighbour." },
  { t: 93, text: "Larger ones walk pgvector's HNSW graph: layers of neighbour links, searched greedily from the top.", say: "Larger ones walk pgvector's H N S W graph: layers of neighbour links, searched greedily from the top." },
  { t: 96.8, text: "Each node gets level ⌊−ln U · m_L⌋ with m_L = 1/ln 16. Every hit is rescored exactly.", say: "Each node gets a random level from an exponential distribution, and every hit is rescored exactly." },

  { t: 100, text: "Every Arkiv entity has an expiry block, and the daemon tracks it." },
  { t: 102.6, text: `fish expires at block ${N + 4}. The chain keeps moving.`, say: "Fish expires four blocks later. The chain keeps moving." },
  { t: 105, text: "Once the chain passes that block, fish leaves the live bitmap. Nobody had to send a delete." },

  { t: 108, text: "Sometimes the chain replaces its newest block. That is a reorg." },
  { t: 110.6, text: "The syncer keeps a ring of recent block hashes and sees that the parent hash no longer matches." },
  { t: 113.2, text: "It rolls the index back to the last common block, then replays the new branch." },

  { t: 118, text: "On chain, each item is a plain Arkiv entity, readable by any Arkiv client." },
  { t: 121, text: "av.vec holds the vector, av.doc the text, and arkiv.schema names the collection for the syncer.", say: "The vector attribute holds the vector, the doc attribute holds the text, and the schema attribute names the collection." },
  { t: 124.4, text: "The distances are pgvector's six operators, compiled from the pgvector 0.8.7 source.", say: "The distances are pgvector's six operators, compiled from the pgvector source." },
  { t: 128.4, text: "Same math as pgvector. The data lives on Arkiv." },
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
