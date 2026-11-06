/**
 * Arkivector client
 * Docs: docs/api/sdk-guidelines.md §2, §3, docs/api/operations.md, docs/api/collections.md
 */

import { ApiError, NotImplementedByDaemonError } from "./errors.js";
import { HttpTransport } from "./http.js";
import {
  ClientOptions,
  VersionInfo,
  Readiness,
  SyncStatus,
  Collection,
  CreateCollectionOptions,
  FollowCollectionOptions,
  Entity,
  SearchResult,
  BatchResult,
  ScrollPage,
  CountResult,
  Consistency,
  SearchOptions,
  ScrollOptions,
  CountOptions,
  GetEntityOptions,
  parseVersionInfo,
  parseReadiness,
  parseSyncStatus,
  parseCollection,
  serializeCollectionCreate,
  serializeFollowRequest,
  serializeConsistency,
  parseEntity,
  parseSearchResult,
  parseBatchResult,
  parseScrollPage,
  parseCountResult,
} from "./models.js";
import { Vector, HalfVector, Bit, SparseVector } from "./types.js";

/**
 * Arkivector SDK client.
 */
export class Arkivector {
  readonly url: string;
  readonly apiKey?: string;
  readonly timeoutMs: number;
  readonly maxRetries: number;
  readonly sessionConsistency: boolean;

  private readonly transport: HttpTransport;
  private cachedVersion: VersionInfo | null = null;
  private readonly columnTypeCache = new Map<string, string>();

  constructor(options?: ClientOptions) {
    const rawUrl =
      options?.url ??
      (typeof process !== "undefined"
        ? process.env?.ARKIVECTOR_URL
        : undefined) ??
      "http://127.0.0.1:8080";
    this.url = rawUrl.replace(/\/+$/, "");

    this.apiKey =
      options?.apiKey ??
      (typeof process !== "undefined"
        ? process.env?.ARKIVECTOR_API_KEY
        : undefined);

    this.timeoutMs = options?.timeoutMs ?? options?.timeout ?? 10000;
    this.maxRetries = options?.maxRetries ?? 3;
    this.sessionConsistency = options?.sessionConsistency ?? false;

    this.transport = new HttpTransport({
      baseUrl: this.url,
      apiKey: this.apiKey,
      timeoutMs: this.timeoutMs,
      maxRetries: this.maxRetries,
      fetchFn: options?.fetch,
      userAgent: options?.userAgent ?? "@arkivector/client/0.1.0",
    });
  }

  /**
   * Lazily fetch and cache daemon version and capabilities.
   * Validates that api_version major === 1.
   */
  async version(): Promise<VersionInfo> {
    if (this.cachedVersion) {
      return this.cachedVersion;
    }

    const raw =
      await this.transport.get<Record<string, unknown>>("/v1/version");
    const info = parseVersionInfo(raw);

    const major = info.apiVersion.split(".")[0];
    if (major !== "1") {
      throw new ApiError(
        `Unsupported daemon API version: ${info.apiVersion}. SDK requires major version 1.`,
        { code: "unsupported_api_version" },
      );
    }

    this.cachedVersion = info;
    return info;
  }

  /**
   * Check whether daemon supports the named capability.
   */
  async hasCapability(name: string): Promise<boolean> {
    const v = await this.version();
    return v.capabilities.includes(name);
  }

  /**
   * Check capability locally; throw NotImplementedByDaemonError if missing.
   */
  async checkCapability(name: string): Promise<void> {
    const has = await this.hasCapability(name);
    if (!has) {
      throw new NotImplementedByDaemonError(
        `Daemon does not implement capability '${name}'`,
        { code: "not_implemented", retryable: false },
      );
    }
  }

  /**
   * Process liveness probe: GET /healthz.
   * Returns true if status is "ok", false on any failure.
   */
  async health(): Promise<boolean> {
    try {
      const res = await this.transport.get<{ status?: string }>("/healthz", {
        maxRetries: 0,
      });
      return res?.status === "ok";
    } catch {
      return false;
    }
  }

  /**
   * Readiness probe: GET /readyz.
   * Returns Readiness object (ready: boolean, collections: Record<string, string>).
   */
  async ready(): Promise<Readiness> {
    try {
      const raw = await this.transport.get<Record<string, unknown>>("/readyz", {
        maxRetries: 0,
      });
      return parseReadiness(raw);
    } catch (err: unknown) {
      if (
        err instanceof ApiError &&
        err.status === 503 &&
        err.raw &&
        typeof err.raw === "object"
      ) {
        const rawObj = err.raw as Record<string, unknown>;
        if ("collections" in rawObj) {
          return parseReadiness(rawObj);
        }
      }
      throw err;
    }
  }

  /**
   * Get sync status of all followed collections: GET /v1/sync.
   */
  async syncStatus(): Promise<SyncStatus[]> {
    await this.checkCapability("ops.sync");
    const raw = await this.transport.get<{
      collections?: Record<string, unknown>[];
    }>("/v1/sync");
    const list = Array.isArray(raw?.collections) ? raw.collections : [];
    return list.map(parseSyncStatus);
  }

  /**
   * List all collections on this daemon: GET /v1/collections.
   */
  async listCollections(): Promise<Collection[]> {
    await this.checkCapability("collections.read");
    const raw = await this.transport.get<{
      collections?: Record<string, unknown>[];
    }>("/v1/collections");
    const list = Array.isArray(raw?.collections) ? raw.collections : [];
    const collections = list.map(parseCollection);
    for (const c of collections) {
      this.columnTypeCache.set(c.name, c.column.type);
    }
    return collections;
  }

  /**
   * Describe one collection: GET /v1/collections/{name}.
   * Caches column type per collection name.
   */
  async getCollection(name: string): Promise<Collection> {
    await this.checkCapability("collections.read");
    const raw = await this.transport.get<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(name)}`,
    );
    const collection = parseCollection(raw);
    this.columnTypeCache.set(collection.name, collection.column.type);
    return collection;
  }

  /**
   * Define a collection and start syncing: POST /v1/collections.
   */
  async createCollection(
    options: CreateCollectionOptions,
  ): Promise<Collection> {
    if (options.publishManifest || options.publish_manifest) {
      await this.checkCapability("collections.manifest");
    } else {
      await this.checkCapability("collections.write");
    }

    const body = serializeCollectionCreate(options);
    const raw = await this.transport.post<Record<string, unknown>>(
      "/v1/collections",
      body,
    );
    const collection = parseCollection(raw);
    this.columnTypeCache.set(collection.name, collection.column.type);
    return collection;
  }

  /**
   * Follow an existing collection: POST /v1/collections:follow.
   */
  async followCollection(
    options: FollowCollectionOptions,
  ): Promise<Collection> {
    await this.checkCapability("collections.follow");
    const body = serializeFollowRequest(options);
    const raw = await this.transport.post<Record<string, unknown>>(
      "/v1/collections:follow",
      body,
    );
    const collection = parseCollection(raw);
    this.columnTypeCache.set(collection.name, collection.column.type);
    return collection;
  }

  /**
   * Stop following a collection and drop local index: DELETE /v1/collections/{name}.
   */
  async deleteCollection(name: string): Promise<void> {
    await this.checkCapability("collections.write");
    await this.transport.delete(`/v1/collections/${encodeURIComponent(name)}`);
    this.columnTypeCache.delete(name);
  }

  /**
   * Retrieve cached column type for a collection.
   */
  getColumnType(name: string): string | undefined {
    return this.columnTypeCache.get(name);
  }

  /**
   * Manually set cached column type for a collection.
   */
  setColumnType(name: string, type: string): void {
    this.columnTypeCache.set(name, type);
  }

  /**
   * Point lookup of an entity by key or external id: GET /v1/collections/{name}/entities/{ref}.
   * Docs: docs/api/entities.md §2
   */
  async get(
    collection: string,
    ref: string,
    options?: GetEntityOptions,
  ): Promise<Entity> {
    await this.checkCapability("entities.read");
    if (options?.verify) {
      await this.checkCapability("entities.verify");
    }

    const isKey = /^0x[0-9a-fA-F]{64}$/.test(ref);
    const refSegment = isKey ? ref : encodeURIComponent(ref);

    const params = new URLSearchParams();
    if (options?.include && options.include.length > 0) {
      params.set("include", options.include.join(","));
    }
    if (options?.verify !== undefined) {
      params.set("verify", String(options.verify));
    }
    const minBlock =
      options?.minBlock ??
      options?.min_block ??
      options?.consistency?.minBlock ??
      options?.consistency?.min_block;
    if (minBlock !== undefined) {
      params.set("min_block", String(minBlock));
    }
    const timeoutMs =
      options?.timeoutMs ??
      options?.timeout_ms ??
      options?.consistency?.timeoutMs ??
      options?.consistency?.timeout_ms;
    if (timeoutMs !== undefined) {
      params.set("timeout_ms", String(timeoutMs));
    }

    const qs = params.toString();
    const path = `/v1/collections/${encodeURIComponent(collection)}/entities/${refSegment}${qs ? `?${qs}` : ""}`;
    const raw = await this.transport.get<Record<string, unknown>>(path);

    if (raw && typeof raw === "object" && "entity" in raw && raw.entity) {
      return parseEntity(raw.entity as Record<string, unknown>);
    }
    return parseEntity(raw);
  }

  /**
   * Vector similarity search: POST /v1/collections/{name}/search.
   * Docs: docs/api/search.md §1
   */
  async search(
    collection: string,
    options: SearchOptions,
  ): Promise<SearchResult> {
    let colType = this.getColumnType(collection);
    if (!colType) {
      try {
        const col = await this.getCollection(collection);
        colType = col.column.type;
      } catch {
        // Continue if collection type cannot be retrieved
      }
    }

    const body = serializeSearchRequest(options, colType);
    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/search`,
      body,
    );
    return parseSearchResult(raw);
  }

  /**
   * Batch search across multiple query vectors: POST /v1/collections/{name}/search:batch.
   * Docs: docs/api/search.md §2
   */
  async searchBatch(
    collection: string,
    searches: SearchOptions[],
    consistency?: Consistency,
  ): Promise<BatchResult> {
    let colType = this.getColumnType(collection);
    if (!colType) {
      try {
        const col = await this.getCollection(collection);
        colType = col.column.type;
      } catch {
        // Continue if collection type cannot be retrieved
      }
    }

    const serializedSearches = searches.map((s) => {
      const sBody = serializeSearchRequest(s, colType);
      delete sBody.consistency; // per-search consistency is ignored in batch per search.md §2
      return sBody;
    });

    const body: Record<string, unknown> = {
      searches: serializedSearches,
    };
    const cons = consistency ? serializeConsistency(consistency) : undefined;
    if (cons !== undefined) {
      body.consistency = cons;
    }

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/search:batch`,
      body,
    );
    return parseBatchResult(raw);
  }

  /**
   * Fetch one page of live entities: POST /v1/collections/{name}/scroll.
   * Docs: docs/api/search.md §3
   */
  async scrollPage(
    collection: string,
    options?: ScrollOptions,
  ): Promise<ScrollPage> {
    await this.checkCapability("scroll");

    const body: Record<string, unknown> = {};
    if (options?.filter !== undefined) body.filter = options.filter;
    if (options?.where !== undefined) body.where = options.where;
    if (options?.limit !== undefined) body.limit = options.limit;
    if (options?.cursor !== undefined) body.cursor = options.cursor;
    if (options?.include !== undefined) body.include = options.include;
    const consistency = options?.consistency
      ? serializeConsistency(options.consistency)
      : undefined;
    if (consistency !== undefined) body.consistency = consistency;

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/scroll`,
      body,
    );
    return parseScrollPage(raw);
  }

  /**
   * Auto-paging async generator over live entities: POST /v1/collections/{name}/scroll.
   * Docs: docs/api/search.md §3
   */
  async *scroll(
    collection: string,
    options?: ScrollOptions,
  ): AsyncIterable<Entity> {
    await this.checkCapability("scroll");
    let cursor: string | null = options?.cursor ?? null;
    while (true) {
      const page: ScrollPage = await this.scrollPage(collection, {
        ...options,
        cursor: cursor ?? undefined,
      });
      for (const entity of page.entities) {
        yield entity;
      }
      if (!page.nextCursor) {
        break;
      }
      cursor = page.nextCursor;
    }
  }

  /**
   * Exact count of live matching entities: POST /v1/collections/{name}/count.
   * Docs: docs/api/search.md §4
   */
  async count(
    collection: string,
    options?: CountOptions,
  ): Promise<CountResult> {
    await this.checkCapability("count");

    const body: Record<string, unknown> = {};
    if (options?.filter !== undefined) body.filter = options.filter;
    if (options?.where !== undefined) body.where = options.where;
    const consistency = options?.consistency
      ? serializeConsistency(options.consistency)
      : undefined;
    if (consistency !== undefined) body.consistency = consistency;

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/count`,
      body,
    );
    return parseCountResult(raw);
  }
}

/**
 * Serialize SearchOptions into wire JSON SearchRequest format.
 */
function serializeSearchRequest(
  options: SearchOptions,
  colType?: string,
): Record<string, unknown> {
  const body: Record<string, unknown> = {
    k: options.k ?? 10,
  };

  // Vector serialization:
  // If vector is a typed vector whose type matches collection column type, send vector_bytes base64.
  // Otherwise send as JSON vector: [...].
  if (options.vectorBytes !== undefined || options.vector_bytes !== undefined) {
    const vb = options.vectorBytes ?? options.vector_bytes;
    if (typeof vb === "string") {
      body.vector_bytes = vb;
    } else if (vb instanceof Uint8Array) {
      body.vector_bytes = Buffer.from(vb).toString("base64");
    }
  } else if (options.vector !== undefined) {
    const v = options.vector;
    let matchesType = false;
    if (colType) {
      if (colType === "halfvec" && v instanceof HalfVector) matchesType = true;
      else if (colType === "vector" && v instanceof Vector) matchesType = true;
      else if (colType === "bit" && v instanceof Bit) matchesType = true;
      else if (colType === "sparsevec" && v instanceof SparseVector)
        matchesType = true;
    }

    if (
      matchesType &&
      (v instanceof HalfVector ||
        v instanceof Vector ||
        v instanceof Bit ||
        v instanceof SparseVector)
    ) {
      body.vector_bytes = Buffer.from(v.toVectorBytes()).toString("base64");
    } else if (v instanceof HalfVector || v instanceof Vector) {
      body.vector = v.toList();
    } else if (v instanceof Bit) {
      body.vector = v.toList();
    } else if (v instanceof SparseVector) {
      body.vector = {
        dim: v.dim,
        indices: Array.from(v.indices),
        values: Array.from(v.values),
      };
    } else if (Array.isArray(v)) {
      body.vector = v;
    } else if (v instanceof Float32Array || v instanceof Float64Array) {
      body.vector = Array.from(v);
    } else {
      body.vector = v;
    }
  }

  if (options.offset !== undefined) body.offset = options.offset;
  if (options.operator !== undefined) body.operator = options.operator;
  if (options.filter !== undefined) body.filter = options.filter;
  if (options.where !== undefined) body.where = options.where;

  const efSearch = options.efSearch ?? options.ef_search;
  if (efSearch !== undefined) body.ef_search = efSearch;

  const iterativeScan = options.iterativeScan ?? options.iterative_scan;
  if (iterativeScan !== undefined) body.iterative_scan = iterativeScan;

  const maxScanTuples = options.maxScanTuples ?? options.max_scan_tuples;
  if (maxScanTuples !== undefined) body.max_scan_tuples = maxScanTuples;

  const maxDistance = options.maxDistance ?? options.max_distance;
  if (maxDistance !== undefined) body.max_distance = maxDistance;

  const minScore = options.minScore ?? options.min_score;
  if (minScore !== undefined) body.min_score = minScore;

  if (options.exact !== undefined) body.exact = options.exact;
  if (options.include !== undefined) body.include = options.include;

  const consistency = options.consistency
    ? serializeConsistency(options.consistency)
    : undefined;
  if (consistency !== undefined) body.consistency = consistency;

  if (options.explain !== undefined) body.explain = options.explain;

  return body;
}

/**
 * Alias Client = Arkivector.
 */
export const Client = Arkivector;
export type Client = Arkivector;
