# LangChain.js

The package exports a LangChain.js-compatible vector store from `@dikshitrj/arkivector/langchain`. It works with any embeddings object that has `embedDocuments` and `embedQuery` (LangChain's `Embeddings` classes do).

```ts
import { Arkivector } from "@dikshitrj/arkivector";
import { ArkivectorVectorStore } from "@dikshitrj/arkivector/langchain";

const client = new Arkivector();
const store = new ArkivectorVectorStore(embeddings, { client, collection: "notes" });

await store.addDocuments([
  { pageContent: "Cats purr.", metadata: { topic: "pets" } },
  { pageContent: "Dogs bark.", metadata: { topic: "pets" } },
]);
const docs = await store.similaritySearch("which animal purrs?", 1);
const withScores = await store.similaritySearchVectorWithScore(await embeddings.embedQuery("cats"), 2);
await store.delete({ ids: ["…"] });
```

Config: `client`, `collection`, optional `operator`, `filter` or `where` (applied to every search). Page content becomes the entity document and metadata becomes attributes. The collection must exist with the embedding model's dimension.

A runnable example without external services is in [`sdk/node/examples/rag_langchain.ts`](https://github.com/DikshitRJ/arkivector/blob/main/sdk/node/examples/rag_langchain.ts).
