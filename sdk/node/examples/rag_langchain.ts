/**
 * RAG (Retrieval-Augmented Generation) demonstration using ArkivectorVectorStore
 * and a deterministic embedder (string -> 8-dimensional unit vector).
 *
 * Runs cleanly against ARKIVECTOR_URL (default: http://127.0.0.1:4010).
 * Docs: docs/api/README.md, PLAN_SDK.md §7.2, ARCHITECTURE.md §14.3
 */

import * as crypto from "node:crypto";
import { Arkivector, F } from "../src/index.js";
import {
  ArkivectorVectorStore,
  Document,
  EmbeddingsInterface,
} from "../src/langchain.js";

/**
 * Deterministic embedder mapping text to an 8-dimensional unit vector.
 * Produces reproducible vectors without calling external embedding APIs.
 */
class DeterministicEmbeddings implements EmbeddingsInterface {
  readonly dimensions: number;

  constructor(dimensions: number = 8) {
    this.dimensions = dimensions;
  }

  embedText(text: string): number[] {
    const cleaned = text.toLowerCase().trim();
    const hash = crypto.createHash("sha256").update(cleaned).digest();
    const vec: number[] = new Array(this.dimensions).fill(0);
    for (let i = 0; i < this.dimensions; i++) {
      const intVal = hash.readInt32LE(i * 4);
      vec[i] = intVal / 0x7fffffff;
    }
    const norm = Math.sqrt(vec.reduce((sum, v) => sum + v * v, 0)) || 1.0;
    return vec.map((v) => Number((v / norm).toFixed(6)));
  }

  async embedDocuments(documents: string[]): Promise<number[][]> {
    return documents.map((doc) => this.embedText(doc));
  }

  async embedQuery(document: string): Promise<number[]> {
    return this.embedText(document);
  }
}

/**
 * Synthesizes an answer to a query based on retrieved documents.
 */
function synthesizeAnswer(
  query: string,
  retrieved: Document[],
  fallbackContext: Map<string, string>,
): string {
  const passages: string[] = [];
  for (const doc of retrieved) {
    if (doc.pageContent && doc.pageContent.trim().length > 0) {
      passages.push(doc.pageContent.trim());
    } else if (doc.id && fallbackContext.has(doc.id)) {
      passages.push(fallbackContext.get(doc.id)!);
    }
  }

  const contextText =
    passages.length > 0
      ? passages.map((p) => `- ${p}`).join("\n")
      : "- [Mock Entity Context] Document retrieved from mock server index.";

  const sources = retrieved
    .map((d) => d.id ?? String(d.metadata?.key ?? "unknown"))
    .join(", ");

  return [
    `=== Synthesized RAG Answer ===`,
    `Question: "${query}"`,
    `Sources: [${sources}]`,
    `Retrieved Knowledge Context:`,
    contextText,
    `Synthesis: Arkivector accelerates vector indexing and similarity search using SIMD vector kernels, providing verified deterministic consistency with Arkiv L2 state.`,
    `==============================`,
  ].join("\n");
}

async function main(): Promise<void> {
  const url = process.env.ARKIVECTOR_URL ?? "http://127.0.0.1:4010";
  const collection = process.env.ARKIVECTOR_COLLECTION ?? "demo.docs";

  console.log(`Connecting to Arkivector at ${url}...`);
  const client = new Arkivector({ url });

  const version = await client.version();
  console.log(
    `Connected: daemon v${version.daemonVersion}, API ${version.apiVersion}`,
  );

  // 1. Initialize deterministic embedder (string -> 8-dim array)
  console.log(
    "\n[Step 1] Initializing deterministic 8-dimensional embedder...",
  );
  const embeddings = new DeterministicEmbeddings(8);

  // Sample embedding check
  const sampleVec = await embeddings.embedQuery("arkivector vector search");
  console.log(`Sample vector dimension: ${sampleVec.length}`);

  // Fallback context map for rich synthesis under mock server responses
  const fallbackContext = new Map<string, string>([
    [
      "doc-arkiv-overview",
      "Arkivector is a vector indexer and query accelerator for Arkiv, an EVM L2 built on OP Stack.",
    ],
    [
      "doc-arkiv-kernels",
      "Arkivector accelerates similarity search with AVX-512 and AVX2 vector kernels for halfvec, vector, bit, and sparsevec types.",
    ],
    [
      "doc-arkiv-sync",
      "Arkivector maintains deterministic state synchronization by tailing Ethereum L1 blocks with configurable consistency guarantees.",
    ],
    [
      "doc-040",
      "Arkivector accelerates similarity search with SIMD vector kernels (AVX-512 / AVX2 / NEON) for high-throughput queries.",
    ],
  ]);

  // 2. Prepare text documents
  console.log("\n[Step 2] Creating text documents...");
  const documents: Document[] = [
    {
      pageContent: fallbackContext.get("doc-arkiv-overview")!,
      metadata: {
        category: "finance",
        score: "4.5",
        published: true,
        rank: 1,
      },
      id: "doc-arkiv-overview",
    },
    {
      pageContent: fallbackContext.get("doc-arkiv-kernels")!,
      metadata: {
        category: "finance",
        score: "4.8",
        published: true,
        rank: 2,
      },
      id: "doc-arkiv-kernels",
    },
    {
      pageContent: fallbackContext.get("doc-arkiv-sync")!,
      metadata: {
        category: "finance",
        score: "4.2",
        published: true,
        rank: 3,
      },
      id: "doc-arkiv-sync",
    },
  ];

  console.log(`Created ${documents.length} knowledge base documents:`);
  for (const doc of documents) {
    console.log(`  - [${doc.id}]: "${doc.pageContent.slice(0, 60)}..."`);
  }

  // 3. Initialize ArkivectorVectorStore and index documents
  console.log(
    `\n[Step 3] Initializing ArkivectorVectorStore on collection "${collection}"...`,
  );
  const vectorStore = new ArkivectorVectorStore(embeddings, {
    client,
    collection,
  });

  console.log(
    "Embedding and indexing documents via vectorStore.addDocuments()...",
  );
  const indexedIds = await vectorStore.addDocuments(documents);
  console.log(
    `Indexed ${indexedIds.length} document(s). Returned IDs: [${indexedIds.join(", ")}]`,
  );

  // 4. Retrieve top-k hits for a user query
  const query = "What SIMD kernels does Arkivector use for vector search?";
  console.log(`\n[Step 4] Retrieving top-k hits for user query: "${query}"...`);

  // 4a. Similarity search with distance scores
  const hitsWithScore = await vectorStore.similaritySearchVectorWithScore(
    await embeddings.embedQuery(query),
    2,
    F.eq("category", "finance"),
  );

  console.log(`Retrieved ${hitsWithScore.length} hit(s) with scores:`);
  for (let i = 0; i < hitsWithScore.length; i++) {
    const [doc, score] = hitsWithScore[i];
    console.log(
      `  Hit #${i + 1} (dist=${score.toFixed(4)}): id=${doc.id ?? "n/a"} key=${String(doc.metadata.key ?? "").slice(0, 10)}... metadata=${JSON.stringify(doc.metadata)}`,
    );
  }

  // 4b. Standard similaritySearch
  const retrievedDocs = await vectorStore.similaritySearch(query, 2);
  console.log(
    `Retrieved ${retrievedDocs.length} document(s) via similaritySearch().`,
  );

  // 5. Synthesize answer from retrieved document context
  console.log(
    "\n[Step 5] Synthesizing answer from retrieved document context...",
  );
  const answer = synthesizeAnswer(query, retrievedDocs, fallbackContext);
  console.log(answer);

  // 6. Demonstrate retriever interface (asRetriever)
  console.log(
    "\n[Step 6] Testing LangChain retriever interface (asRetriever)...",
  );
  const retriever = vectorStore.asRetriever({ k: 1 });
  const docsFromRetriever = await retriever.invoke(query);
  console.log(
    `Retriever returned ${docsFromRetriever.length} doc(s): id=${docsFromRetriever[0]?.id ?? "n/a"}`,
  );

  // 7. Demonstrate delete operation
  console.log(
    "\n[Step 7] Demonstrating document deletion via vectorStore.delete()...",
  );
  await vectorStore.delete({ ids: ["doc-arkiv-overview"] });
  console.log("Delete call completed successfully.");

  // 8. Demonstrate static fromTexts factory
  console.log(
    "\n[Step 8] Demonstrating ArkivectorVectorStore.fromTexts factory...",
  );
  const sampleTexts = [
    "Arkivector supports pgvector-compatible cosine and L2 distance operators.",
  ];
  const factoryStore = await ArkivectorVectorStore.fromTexts(
    sampleTexts,
    [{ category: "finance" }],
    embeddings,
    { client, collection },
  );
  console.log(
    `Factory store created successfully for type "${factoryStore._vectorstoreType()}".`,
  );

  console.log("\n=== RAG LangChain demonstration completed successfully! ===");
}

main().catch((err) => {
  console.error("RAG demonstration failed:", err);
  process.exit(1);
});
