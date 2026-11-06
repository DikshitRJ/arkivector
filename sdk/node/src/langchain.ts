/**
 * Arkivector VectorStore adapter for LangChain.js.
 * Docs: docs/api/README.md, PLAN_SDK.md §7.2, ARCHITECTURE.md §14.3
 *
 * Compatible with @langchain/core (optional peer dependency).
 */

import * as crypto from "node:crypto";
import { Arkivector } from "./client.js";
import { F, Filter } from "./filter.js";
import { EntityInput, SearchOptions, UpsertOptions } from "./models.js";

/**
 * Compatible Document interface matching @langchain/core/documents.
 */
export interface Document<
  Metadata extends Record<string, unknown> = Record<string, unknown>,
> {
  pageContent: string;
  metadata: Metadata;
  id?: string;
}

/**
 * Compatible Embeddings interface matching @langchain/core/embeddings.
 */
export interface EmbeddingsInterface {
  embedDocuments(documents: string[]): Promise<number[][]>;
  embedQuery(document: string): Promise<number[]>;
}

/**
 * Configuration options for ArkivectorVectorStore.
 */
export interface ArkivectorStoreConfig {
  client: Arkivector;
  collection: string;
  operator?: "<->" | "<#>" | "<=>" | "<+>" | "<~>" | "<%>";
  filter?: Filter | Record<string, unknown>;
  where?: string;
  ids?: string[];
  options?: Record<string, unknown>;
}

/**
 * Delete parameters accepted by ArkivectorVectorStore.delete().
 */
export interface DeleteParams {
  ids?: string[];
  filter?: Filter | Record<string, unknown>;
  where?: string;
}

/**
 * Normalizes user-supplied filter into an Arkivector Filter AST node.
 * Accepts either an existing Filter AST node or a dict-of-equals (Record<string, unknown>).
 */
export function normalizeFilter(
  filter?: Filter | Record<string, unknown> | null,
): Filter | undefined {
  if (!filter || typeof filter !== "object") {
    return undefined;
  }
  const opKeys = new Set([
    "and",
    "or",
    "not",
    "eq",
    "ne",
    "lt",
    "lte",
    "gt",
    "gte",
    "between",
    "startswith",
    "exists",
    "typeof",
  ]);
  const entries = Object.entries(filter);
  if (entries.length === 1 && opKeys.has(entries[0][0])) {
    return filter as Filter;
  }
  // Dict-of-equals: e.g. { category: "finance", rank: 10 }
  const eqFilters: Filter[] = [];
  for (const [key, val] of entries) {
    if (val !== undefined) {
      eqFilters.push(F.eq(key, val));
    }
  }
  if (eqFilters.length === 0) {
    return undefined;
  }
  if (eqFilters.length === 1) {
    return eqFilters[0];
  }
  return F.and(...eqFilters);
}

/**
 * ArkivectorVectorStore implements LangChain.js VectorStore interface
 * backed by an Arkivector collection.
 */
export class ArkivectorVectorStore {
  readonly client: Arkivector;
  readonly collection: string;
  readonly embeddings: EmbeddingsInterface;
  readonly operator?: "<->" | "<#>" | "<=>" | "<+>" | "<~>" | "<%>";
  readonly filter?: Filter;
  readonly where?: string;

  _vectorstoreType(): string {
    return "arkivector";
  }

  constructor(
    embeddings:
      | EmbeddingsInterface
      | {
          embedDocuments: (d: string[]) => Promise<number[][]>;
          embedQuery: (q: string) => Promise<number[]>;
        },
    config: ArkivectorStoreConfig,
  ) {
    if (
      !embeddings ||
      typeof embeddings.embedDocuments !== "function" ||
      typeof embeddings.embedQuery !== "function"
    ) {
      throw new Error(
        "ArkivectorVectorStore: embeddings instance with embedDocuments and embedQuery is required",
      );
    }
    if (!config || typeof config !== "object") {
      throw new Error("ArkivectorVectorStore: config object is required");
    }
    if (!config.client) {
      throw new Error("ArkivectorVectorStore: config.client is required");
    }
    if (!config.collection) {
      throw new Error("ArkivectorVectorStore: config.collection is required");
    }

    this.embeddings = embeddings;
    this.client = config.client;
    this.collection = config.collection;
    this.operator = config.operator;
    this.filter = normalizeFilter(config.filter);
    this.where = config.where;
  }

  /**
   * Add vectors and their corresponding documents to Arkivector.
   * Returns array of entity IDs.
   */
  async addVectors(
    vectors: number[][],
    documents: (Document | Record<string, unknown> | string)[],
    options?: Record<string, unknown>,
  ): Promise<string[]> {
    if (vectors.length !== documents.length) {
      throw new Error(
        `addVectors: vectors length (${vectors.length}) does not match documents length (${documents.length})`,
      );
    }
    if (vectors.length === 0) {
      return [];
    }

    const explicitIds = (
      Array.isArray(options?.ids) ? options.ids : undefined
    ) as (string | undefined)[] | undefined;

    const entities: EntityInput[] = [];
    const generatedIds: string[] = [];

    for (let i = 0; i < documents.length; i++) {
      const doc = documents[i];
      let pageContent = "";
      let metadata: Record<string, unknown> = {};
      let docId: string | undefined;

      if (typeof doc === "string") {
        pageContent = doc;
      } else if (doc && typeof doc === "object") {
        const d = doc as Record<string, unknown>;
        pageContent = String(d.pageContent ?? d.content ?? "");
        if (d.metadata && typeof d.metadata === "object") {
          metadata = { ...(d.metadata as Record<string, unknown>) };
        }
        if (d.id !== undefined && d.id !== null) {
          docId = String(d.id);
        }
      }

      if (explicitIds && explicitIds[i] !== undefined) {
        docId = String(explicitIds[i]);
      } else if (!docId && metadata.id !== undefined && metadata.id !== null) {
        docId = String(metadata.id);
      } else if (!docId) {
        docId = crypto.randomUUID();
      }

      generatedIds.push(docId);

      entities.push({
        id: docId,
        vector: vectors[i],
        document: pageContent,
        attributes: metadata,
      });
    }

    const upsertOptions: UpsertOptions = {};
    if (options?.wait !== undefined) {
      upsertOptions.wait = options.wait as UpsertOptions["wait"];
    }
    if (options?.timeoutMs !== undefined) {
      upsertOptions.timeoutMs = Number(options.timeoutMs);
    }
    if (options?.timeout_ms !== undefined) {
      upsertOptions.timeout_ms = Number(options.timeout_ms);
    }

    const res = await this.client.upsert(
      this.collection,
      entities,
      upsertOptions,
    );

    const returnedIds: string[] = [];
    for (let i = 0; i < entities.length; i++) {
      const item = res.results[i];
      returnedIds.push(item?.id ?? generatedIds[i] ?? item?.key);
    }
    return returnedIds;
  }

  /**
   * Add documents to Arkivector by embedding their pageContent.
   */
  async addDocuments(
    documents: (Document | Record<string, unknown> | string)[],
    options?: Record<string, unknown>,
  ): Promise<string[]> {
    if (documents.length === 0) {
      return [];
    }
    const texts = documents.map((doc) => {
      if (typeof doc === "string") {
        return doc;
      }
      if (doc && typeof doc === "object") {
        const d = doc as Record<string, unknown>;
        return String(d.pageContent ?? d.content ?? "");
      }
      return "";
    });

    const vectors = await this.embeddings.embedDocuments(texts);
    return this.addVectors(vectors, documents, options);
  }

  /**
   * Search for similar vectors and return documents with their distance scores.
   */
  async similaritySearchVectorWithScore(
    query: number[],
    k: number = 4,
    filter?: Filter | Record<string, unknown>,
  ): Promise<[Document, number][]> {
    if (!Array.isArray(query) || query.length === 0) {
      throw new TypeError(
        "similaritySearchVectorWithScore: query vector must be a non-empty array of numbers",
      );
    }

    const activeFilter =
      filter !== undefined ? normalizeFilter(filter) : this.filter;

    const searchOptions: SearchOptions = {
      vector: query,
      k: Math.max(1, k),
      include: ["attributes", "document"],
    };
    if (activeFilter !== undefined) {
      searchOptions.filter = activeFilter;
    }
    if (this.operator !== undefined) {
      searchOptions.operator = this.operator;
    }
    if (this.where !== undefined) {
      searchOptions.where = this.where;
    }

    const searchResult = await this.client.search(
      this.collection,
      searchOptions,
    );

    return searchResult.hits.map((hit) => {
      const metadata: Record<string, unknown> = {
        ...hit.plainAttributes(),
        key: hit.key,
      };
      if (hit.id) {
        metadata.id = hit.id;
      }
      const doc: Document = {
        pageContent: hit.document ?? "",
        metadata,
        id: hit.id ?? undefined,
      };
      return [doc, hit.distance];
    });
  }

  /**
   * Embed query string and search for the most similar documents.
   */
  async similaritySearch(
    query: string,
    k: number = 4,
    filter?: Filter | Record<string, unknown>,
  ): Promise<Document[]> {
    const queryVector = await this.embeddings.embedQuery(query);
    const results = await this.similaritySearchVectorWithScore(
      queryVector,
      k,
      filter,
    );
    return results.map(([doc]) => doc);
  }

  /**
   * Delete documents by IDs, filter, or where clause.
   */
  async delete(params: DeleteParams): Promise<void> {
    if (!params || typeof params !== "object") {
      throw new Error("delete: params object is required");
    }

    if (params.ids && params.ids.length > 0) {
      if (params.ids.length === 1) {
        await this.client.delete(this.collection, { ref: params.ids[0] });
      } else {
        await this.client.delete(this.collection, { refs: params.ids });
      }
      return;
    }

    if (params.filter !== undefined) {
      const normFilter = normalizeFilter(params.filter);
      if (normFilter) {
        await this.client.delete(this.collection, { filter: normFilter });
        return;
      }
    }

    if (params.where !== undefined) {
      await this.client.delete(this.collection, { where: params.where });
      return;
    }

    throw new Error("delete: must provide ids, filter, or where");
  }

  /**
   * Create an ArkivectorVectorStore instance from raw texts and embed them.
   */
  static async fromTexts(
    texts: string[],
    metadatas: Record<string, unknown>[] | Record<string, unknown>,
    embeddings:
      | EmbeddingsInterface
      | {
          embedDocuments: (d: string[]) => Promise<number[][]>;
          embedQuery: (q: string) => Promise<number[]>;
        },
    dbConfig: ArkivectorStoreConfig,
  ): Promise<ArkivectorVectorStore> {
    const store = new ArkivectorVectorStore(embeddings, dbConfig);
    const docs: Document[] = texts.map((text, idx) => {
      const metadata = Array.isArray(metadatas)
        ? (metadatas[idx] ?? {})
        : metadatas && typeof metadatas === "object"
          ? { ...metadatas }
          : {};
      return {
        pageContent: text,
        metadata: { ...metadata },
      };
    });
    const explicitIds =
      dbConfig?.ids ?? (dbConfig?.options?.ids as string[] | undefined);
    await store.addDocuments(
      docs,
      explicitIds
        ? { ids: explicitIds, ...dbConfig?.options }
        : dbConfig?.options,
    );
    return store;
  }

  /**
   * Create an ArkivectorVectorStore instance from Documents and embed them.
   */
  static async fromDocuments(
    documents: (Document | Record<string, unknown> | string)[],
    embeddings:
      | EmbeddingsInterface
      | {
          embedDocuments: (d: string[]) => Promise<number[][]>;
          embedQuery: (q: string) => Promise<number[]>;
        },
    dbConfig: ArkivectorStoreConfig,
  ): Promise<ArkivectorVectorStore> {
    const store = new ArkivectorVectorStore(embeddings, dbConfig);
    await store.addDocuments(documents, dbConfig?.options);
    return store;
  }

  /**
   * Returns a LangChain-compatible retriever interface.
   */
  asRetriever(
    kOrOptions?:
      number | { k?: number; filter?: Filter | Record<string, unknown> },
  ) {
    const k =
      typeof kOrOptions === "number" ? kOrOptions : (kOrOptions?.k ?? 4);
    const filter =
      typeof kOrOptions === "object" ? kOrOptions?.filter : undefined;
    return {
      getRelevantDocuments: async (query: string): Promise<Document[]> => {
        return this.similaritySearch(query, k, filter);
      },
      invoke: async (query: string): Promise<Document[]> => {
        return this.similaritySearch(query, k, filter);
      },
    };
  }
}
