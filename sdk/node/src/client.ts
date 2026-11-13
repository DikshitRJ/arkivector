/**
 * Arkivector client
 * Docs: docs/api/sdk-guidelines.md §2, §3, docs/api/operations.md, docs/api/collections.md
 */

import * as crypto from "node:crypto";
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
  EntityInput,
  UpsertResult,
  PrepareResult,
  TxResult,
  UpsertOptions,
  PrepareOptions,
  SubmitOptions,
  SubmitResult,
  DeleteEntitiesOptions,
  ExtendEntitiesOptions,
  DirectAttribute,
  TtlInput,
  TypedValue,
  parseVersionInfo,
  parseReadiness,
  parseSyncStatus,
  parseCollection,
  serializeCollectionCreate,
  serializeFollowRequest,
  serializeConsistency,
  serializeTtl,
  parseEntity,
  parseSearchResult,
  parseBatchResult,
  parseScrollPage,
  parseCountResult,
  parseUpsertResult,
  parsePrepareResult,
  parseSubmitResult,
  parseTxResult,
  plainAttributes,
} from "./models.js";
import { Vector, HalfVector, Bit, SparseVector } from "./types.js";
import {
  TransactionSigner,
  classifySigned,
  resolveSigner,
  toHex,
} from "./signing.js";

/**
 * Options for upsertSigned(): prepare → sign with the caller's wallet → submit.
 */
export interface UpsertSignedOptions {
  /** Wallet / signer (see signing.ts): EIP-1193 provider, viem WalletClient, ethers Signer, function. */
  signer: TransactionSigner;
  /** Signing address. Optional when the signer can tell (wallets, viem, ethers). */
  from?: string;
  /** Default "indexed": a following search sees the writes. */
  wait?: "none" | "receipt" | "indexed" | string;
  timeoutMs?: number;
  idempotencyKey?: string;
}

/**
 * Arkivector SDK client.
 */
export class Arkivector {
  readonly url: string;
  readonly apiKey?: string;
  readonly timeoutMs: number;
  readonly maxRetries: number;
  readonly sessionConsistency: boolean;
  readonly idempotency: boolean;

  private readonly transport: HttpTransport;
  private cachedVersion: VersionInfo | null = null;
  private readonly columnTypeCache = new Map<string, string>();
  private _lastWriteBlock: number | null = null;

  /**
   * Latest block number returned by a managed write operation.
   * Docs: docs/api/consistency.md §2
   */
  get lastWriteBlock(): number | null {
    return this._lastWriteBlock;
  }
  get last_write_block(): number | null {
    return this._lastWriteBlock;
  }

  constructor(options?: ClientOptions) {
    const rawUrl =
      options?.url ??
      (typeof process !== "undefined"
        ? process.env?.ARKIVECTOR_URL
        : undefined) ??
      "http://127.0.0.1:8080";
    // Strip trailing slashes without a regex (CodeQL js/polynomial-redos on /\/+$/).
    let url = rawUrl;
    while (url.endsWith("/")) url = url.slice(0, -1);
    this.url = url;

    this.apiKey =
      options?.apiKey ??
      (typeof process !== "undefined"
        ? process.env?.ARKIVECTOR_API_KEY
        : undefined);

    this.timeoutMs = options?.timeoutMs ?? options?.timeout ?? 10000;
    this.maxRetries = options?.maxRetries ?? 3;
    this.sessionConsistency = options?.sessionConsistency ?? false;
    this.idempotency = options?.idempotency ?? true;

    this.transport = new HttpTransport({
      baseUrl: this.url,
      apiKey: this.apiKey,
      timeoutMs: this.timeoutMs,
      maxRetries: this.maxRetries,
      fetchFn: options?.fetch,
      userAgent: options?.userAgent ?? "arkivector-node/1.0",
    });
  }

  private resolveIdempotencyKey(options?: {
    idempotencyKey?: string;
    idempotency_key?: string;
    idempotency?: boolean;
  }): string | undefined {
    const explicit = options?.idempotencyKey ?? options?.idempotency_key;
    if (explicit !== undefined) {
      return explicit;
    }
    const enabled = options?.idempotency ?? this.idempotency;
    if (enabled) {
      return crypto.randomUUID();
    }
    return undefined;
  }

  private updateLastWriteBlock(blockNumber?: number | null): void {
    if (typeof blockNumber === "number" && blockNumber > 0) {
      if (this._lastWriteBlock === null || blockNumber > this._lastWriteBlock) {
        this._lastWriteBlock = blockNumber;
      }
    }
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
    let minBlock =
      options?.minBlock ??
      options?.min_block ??
      options?.consistency?.minBlock ??
      options?.consistency?.min_block;
    if (
      minBlock === undefined &&
      this.sessionConsistency &&
      this._lastWriteBlock !== null
    ) {
      minBlock = this._lastWriteBlock;
    }
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

    let consistency = options.consistency;
    if (this.sessionConsistency && this._lastWriteBlock !== null) {
      const existingMin = consistency?.minBlock ?? consistency?.min_block;
      if (existingMin === undefined) {
        consistency = { ...consistency, minBlock: this._lastWriteBlock };
      }
    }

    const body = serializeSearchRequest({ ...options, consistency }, colType);
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

    let batchConsistency = consistency;
    if (this.sessionConsistency && this._lastWriteBlock !== null) {
      const existingMin =
        batchConsistency?.minBlock ?? batchConsistency?.min_block;
      if (existingMin === undefined) {
        batchConsistency = {
          ...batchConsistency,
          minBlock: this._lastWriteBlock,
        };
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
    const cons = batchConsistency
      ? serializeConsistency(batchConsistency)
      : undefined;
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

    let consistency = options?.consistency;
    if (this.sessionConsistency && this._lastWriteBlock !== null) {
      const existingMin = consistency?.minBlock ?? consistency?.min_block;
      if (existingMin === undefined) {
        consistency = { ...consistency, minBlock: this._lastWriteBlock };
      }
    }

    const body: Record<string, unknown> = {};
    if (options?.filter !== undefined) body.filter = options.filter;
    if (options?.where !== undefined) body.where = options.where;
    if (options?.limit !== undefined) body.limit = options.limit;
    if (options?.cursor !== undefined) body.cursor = options.cursor;
    if (options?.include !== undefined) body.include = options.include;
    const consistencySerialized = consistency
      ? serializeConsistency(consistency)
      : undefined;
    if (consistencySerialized !== undefined)
      body.consistency = consistencySerialized;

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

    let consistency = options?.consistency;
    if (this.sessionConsistency && this._lastWriteBlock !== null) {
      const existingMin = consistency?.minBlock ?? consistency?.min_block;
      if (existingMin === undefined) {
        consistency = { ...consistency, minBlock: this._lastWriteBlock };
      }
    }

    const body: Record<string, unknown> = {};
    if (options?.filter !== undefined) body.filter = options.filter;
    if (options?.where !== undefined) body.where = options.where;
    const consistencySerialized = consistency
      ? serializeConsistency(consistency)
      : undefined;
    if (consistencySerialized !== undefined)
      body.consistency = consistencySerialized;

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/count`,
      body,
    );
    return parseCountResult(raw);
  }

  /**
   * Managed entity upsert: POST /v1/collections/{name}/entities.
   * Docs: docs/api/entities.md §3
   */
  async upsert(
    collection: string,
    entities: EntityInput[],
    options?: UpsertOptions,
  ): Promise<UpsertResult> {
    let colType = this.getColumnType(collection);
    if (!colType) {
      try {
        const col = await this.getCollection(collection);
        colType = col.column.type;
      } catch {
        // Continue if column type cannot be retrieved
      }
    }

    const serializedEntities = entities.map((e) =>
      serializeEntityInput(e, colType),
    );

    const wait = options?.wait ?? "receipt";
    const timeoutMs =
      options?.timeoutMs ??
      options?.timeout_ms ??
      (wait === "none" ? this.timeoutMs : Math.max(this.timeoutMs, 30000));

    const body: Record<string, unknown> = {
      entities: serializedEntities,
      wait,
    };
    if (options?.timeoutMs !== undefined || options?.timeout_ms !== undefined) {
      body.timeout_ms = options.timeoutMs ?? options.timeout_ms;
    }

    const idempotencyKey = this.resolveIdempotencyKey(options);

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/entities`,
      body,
      { idempotencyKey, timeoutMs },
    );

    const result = parseUpsertResult(raw);
    this.updateLastWriteBlock(result.blockNumber);
    return result;
  }

  /**
   * Client-side signing preparation: POST /v1/collections/{name}/entities:prepare.
   * Docs: docs/api/entities.md §4
   */
  async prepare(
    collection: string,
    entities: EntityInput[],
    options: PrepareOptions,
  ): Promise<PrepareResult> {
    let colType = this.getColumnType(collection);
    if (!colType) {
      try {
        const col = await this.getCollection(collection);
        colType = col.column.type;
      } catch {
        // Continue if column type cannot be retrieved
      }
    }

    const serializedEntities = entities.map((e) =>
      serializeEntityInput(e, colType),
    );

    const body: Record<string, unknown> = {
      entities: serializedEntities,
      from: options.from,
    };

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/entities:prepare`,
      body,
      { isIdempotent: true },
    );

    return parsePrepareResult(raw);
  }

  /**
   * Hand transactions signed outside the daemon back to it (contract 1.1.0):
   * POST /v1/collections/{name}/entities:submit. Give exactly one of signedTransactions (the
   * daemon broadcasts them in order) or txHashes (a wallet already broadcast them).
   * Docs: docs/api/entities.md §4.2
   */
  async submit(
    collection: string,
    options: SubmitOptions,
  ): Promise<SubmitResult> {
    await this.checkCapability("entities.submit");
    const hasRaw = options.signedTransactions !== undefined;
    const hasHash = options.txHashes !== undefined;
    if (hasRaw === hasHash) {
      throw new Error(
        "submit: give exactly one of signedTransactions or txHashes",
      );
    }
    const wait = options.wait ?? "receipt";
    const body: Record<string, unknown> = { wait };
    if (hasRaw) {
      const raws = (options.signedTransactions ?? []).map((t) => {
        const c = classifySigned(t);
        if (c.kind !== "raw") {
          throw new Error(
            "submit: signedTransactions must be signed EIP-1559 (0x02…) transactions; pass hashes as txHashes",
          );
        }
        return c.value;
      });
      if (raws.length === 0)
        throw new Error("submit: signedTransactions is empty");
      body.signed_transactions = raws;
    } else {
      const hashes = (options.txHashes ?? []).map((h) => {
        const v = toHex(h);
        if (!/^0x[0-9a-f]{64}$/.test(v)) {
          throw new Error(`submit: ${h} is not a transaction hash`);
        }
        return v;
      });
      if (hashes.length === 0) throw new Error("submit: txHashes is empty");
      body.tx_hashes = hashes;
    }
    if (options.timeoutMs !== undefined) body.timeout_ms = options.timeoutMs;
    const timeoutMs =
      wait === "none"
        ? this.timeoutMs
        : Math.max(this.timeoutMs, (options.timeoutMs ?? 60000) + 5000);
    const idempotencyKey = this.resolveIdempotencyKey(options);
    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/entities:submit`,
      body,
      { idempotencyKey, timeoutMs },
    );
    const result = parseSubmitResult(raw);
    this.updateLastWriteBlock(result.blockNumber);
    return result;
  }

  /**
   * Upsert signed by the caller's own wallet: prepare → sign/send → submit (contract 1.1.0).
   * The daemon needs no key. Works with MetaMask (`window.ethereum`), viem, ethers, or a
   * function. With wait receipt/indexed the returned keys are the mined ones.
   * Docs: docs/api/entities.md §4
   */
  async upsertSigned(
    collection: string,
    entities: EntityInput[],
    options: UpsertSignedOptions,
  ): Promise<UpsertResult> {
    const signer = resolveSigner(options.signer);
    const from = options.from ?? (await signer.address());
    if (!from) {
      throw new Error(
        "upsertSigned: pass options.from (this signer cannot tell its address)",
      );
    }
    const prepared = await this.prepare(collection, entities, { from });
    if (prepared.transactions.length === 0) {
      return new UpsertResult({ results: prepared.results, txHashes: [] });
    }
    await signer.prepareChain(prepared.chainId);
    const signed = [];
    for (const tx of prepared.transactions) signed.push(await signer.sign(tx));
    const raws = signed.filter((s) => s.kind === "raw").map((s) => s.value);
    const hashes = signed.filter((s) => s.kind === "hash").map((s) => s.value);
    if (raws.length > 0 && hashes.length > 0) {
      throw new Error(
        "upsertSigned: the signer broadcast some transactions and only signed others",
      );
    }
    const sub = await this.submit(collection, {
      ...(raws.length > 0
        ? { signedTransactions: raws }
        : { txHashes: hashes }),
      wait: options.wait ?? "indexed",
      timeoutMs: options.timeoutMs,
      idempotencyKey: options.idempotencyKey,
    });
    const results = prepared.results.map((r) => ({ ...r }));
    const creates = results.filter((r) => r.op === "create");
    if (creates.length === sub.createdKeys.length) {
      creates.forEach((r, i) => {
        r.key = sub.createdKeys[i];
      });
    }
    return new UpsertResult({
      results,
      txHashes: sub.txHashes,
      blockNumber: sub.blockNumber,
    });
  }

  /**
   * Delete entity or entities:
   * - DELETE /v1/collections/{name}/entities/{ref} if options.ref
   * - POST /v1/collections/{name}/entities:delete if options.refs/filter/where
   * Docs: docs/api/entities.md §5
   */
  async delete(
    collection: string,
    options: DeleteEntitiesOptions,
  ): Promise<TxResult> {
    const idempotencyKey = this.resolveIdempotencyKey(options);
    const timeoutMs = options.timeoutMs ?? options.timeout_ms;

    let raw: Record<string, unknown>;

    if (options.ref !== undefined) {
      const isKey = /^0x[0-9a-fA-F]{64}$/.test(options.ref);
      const refSegment = isKey ? options.ref : encodeURIComponent(options.ref);
      raw = await this.transport.delete<Record<string, unknown>>(
        `/v1/collections/${encodeURIComponent(collection)}/entities/${refSegment}`,
        { idempotencyKey, timeoutMs },
      );
    } else if (
      options.refs !== undefined ||
      options.filter !== undefined ||
      options.where !== undefined
    ) {
      const body: Record<string, unknown> = {};
      if (options.refs !== undefined) body.refs = options.refs;
      else if (options.filter !== undefined) body.filter = options.filter;
      else if (options.where !== undefined) body.where = options.where;

      raw = await this.transport.post<Record<string, unknown>>(
        `/v1/collections/${encodeURIComponent(collection)}/entities:delete`,
        body,
        { idempotencyKey, timeoutMs },
      );
    } else {
      throw new Error(
        "DeleteEntitiesOptions must specify either ref, refs, filter, or where",
      );
    }

    const result = parseTxResult(raw);
    this.updateLastWriteBlock(result.blockNumber);
    return result;
  }

  /**
   * Extend entity TTL: POST /v1/collections/{name}/entities:extend.
   * Docs: docs/api/entities.md §5
   */
  async extend(
    collection: string,
    ttl: TtlInput,
    options: ExtendEntitiesOptions,
  ): Promise<TxResult> {
    const idempotencyKey = this.resolveIdempotencyKey(options);
    const timeoutMs = options.timeoutMs ?? options.timeout_ms;

    const body: Record<string, unknown> = {
      ttl: serializeTtl(ttl),
    };
    if (options.refs !== undefined) body.refs = options.refs;
    else if (options.filter !== undefined) body.filter = options.filter;
    else if (options.where !== undefined) body.where = options.where;
    else {
      throw new Error(
        "ExtendEntitiesOptions must specify either refs, filter, or where",
      );
    }

    const raw = await this.transport.post<Record<string, unknown>>(
      `/v1/collections/${encodeURIComponent(collection)}/entities:extend`,
      body,
      { idempotencyKey, timeoutMs },
    );

    const result = parseTxResult(raw);
    this.updateLastWriteBlock(result.blockNumber);
    return result;
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

/**
 * Serialize an EntityInput into wire JSON for managed upsert or prepare.
 */
export function serializeEntityInput(
  entity: EntityInput,
  colType?: string,
): Record<string, unknown> {
  const body: Record<string, unknown> = {};

  if (entity.id !== undefined) {
    body.id = entity.id;
  }

  const vb = (entity.vectorBytes ?? entity.vector_bytes) as unknown;
  if (vb !== undefined && vb !== null) {
    if (typeof vb === "string") {
      body.vector_bytes = vb;
    } else if (vb instanceof Uint8Array) {
      body.vector_bytes = Buffer.from(vb).toString("base64");
    }
  } else if (entity.vector !== undefined) {
    const v = entity.vector;
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

  if (entity.document !== undefined) {
    body.document = entity.document;
  }

  if (entity.attributes !== undefined) {
    const attrs: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(entity.attributes)) {
      if (v instanceof TypedValue) {
        attrs[k] = v.toJSON();
      } else if (v && typeof v === "object" && "type" in v && "value" in v) {
        attrs[k] = {
          type: (v as { type: string }).type,
          value: (v as { value: unknown }).value,
        };
      } else {
        attrs[k] = v;
      }
    }
    body.attributes = attrs;
  }

  if (entity.ttl !== undefined) {
    body.ttl = serializeTtl(entity.ttl);
  }

  return body;
}

/**
 * Big-endian 32-byte representation of a 256-bit BigInt.
 */
function bigIntToBytes32(bi: bigint): Uint8Array {
  const buf = new Uint8Array(32);
  let temp = BigInt.asUintN(256, bi);
  for (let i = 31; i >= 0; i--) {
    buf[i] = Number(temp & 0xffn);
    temp >>= 8n;
  }
  return buf;
}

/**
 * Parse decimal string into fixed-point integer scaled by 10^18.
 */
function parseDecToBigInt(s: string): bigint {
  const str = String(s).trim();
  const isNeg = str.startsWith("-");
  const unsignedStr = isNeg ? str.slice(1) : str;
  const [intPart, fracPart = ""] = unsignedStr.split(".");
  if (fracPart.length > 18) {
    throw new RangeError(`Decimal exceeds 18 decimal places: ${s}`);
  }
  const paddedFrac = fracPart.padEnd(18, "0");
  const combined = BigInt(intPart + paddedFrac);
  return isNeg ? -combined : combined;
}

/**
 * Encode an attribute name and value into wire DirectAttribute representation.
 */
export function encodeAttributeValue(
  type?: string,
  value?: unknown,
): { typeId: number; valueBytes: Uint8Array } {
  if (
    value &&
    typeof value === "object" &&
    "typeId" in value &&
    "valueBytes" in value
  ) {
    const rawObj = value as { typeId: number; valueBytes: Uint8Array };
    return { typeId: rawObj.typeId, valueBytes: rawObj.valueBytes };
  }

  let resolvedType = type;
  let val = value;
  if (val instanceof TypedValue) {
    resolvedType = val.type;
    val = val.value;
  } else if (
    val &&
    typeof val === "object" &&
    "type" in val &&
    "value" in val
  ) {
    resolvedType = (val as { type: string }).type;
    val = (val as { value: unknown }).value;
  }

  if (!resolvedType) {
    if (typeof val === "boolean") resolvedType = "bool";
    else if (typeof val === "number")
      resolvedType = Number.isInteger(val) ? "i32" : "dec";
    else if (typeof val === "bigint") resolvedType = "u64";
    else if (val instanceof Uint8Array) resolvedType = "bytes";
    else if (typeof val === "string") {
      if (/^0x[0-9a-fA-F]{40}$/.test(val)) resolvedType = "addr";
      else if (/^0x[0-9a-fA-F]{64}$/.test(val)) resolvedType = "key";
      else resolvedType = "str";
    } else {
      resolvedType = "str";
    }
  }

  switch (resolvedType) {
    case "bool": {
      const buf = new Uint8Array(32);
      if (val) buf[31] = 1;
      return { typeId: 0x01, valueBytes: buf };
    }
    case "i32": {
      const num =
        typeof val === "number" ? Math.trunc(val) : parseInt(String(val), 10);
      const buf = bigIntToBytes32(BigInt.asIntN(256, BigInt(num)));
      return { typeId: 0x02, valueBytes: buf };
    }
    case "u64": {
      const bi = BigInt(val as string | number | bigint);
      return { typeId: 0x03, valueBytes: bigIntToBytes32(bi) };
    }
    case "u256": {
      const bi = BigInt(val as string | number | bigint);
      return { typeId: 0x04, valueBytes: bigIntToBytes32(bi) };
    }
    case "dec": {
      const bi = parseDecToBigInt(String(val));
      return { typeId: 0x05, valueBytes: bigIntToBytes32(bi) };
    }
    case "bytes": {
      if (val instanceof Uint8Array) {
        return { typeId: 0x07, valueBytes: val };
      }
      if (typeof val === "string") {
        return {
          typeId: 0x07,
          valueBytes: Uint8Array.from(Buffer.from(val, "base64")),
        };
      }
      throw new TypeError(`Cannot convert value to bytes: ${String(val)}`);
    }
    case "str": {
      return {
        typeId: 0x08,
        valueBytes: new TextEncoder().encode(String(val ?? "")),
      };
    }
    case "addr": {
      const clean = String(val).toLowerCase().replace(/^0x/, "");
      const addrBuf = Buffer.from(clean, "hex");
      const buf = new Uint8Array(32);
      buf.set(addrBuf, 12);
      return { typeId: 0x09, valueBytes: buf };
    }
    case "key": {
      const clean = String(val).toLowerCase().replace(/^0x/, "");
      const keyBuf = Buffer.from(clean, "hex");
      const buf = new Uint8Array(32);
      buf.set(keyBuf, 0);
      return { typeId: 0x0a, valueBytes: buf };
    }
    default: {
      return {
        typeId: 0x08,
        valueBytes: new TextEncoder().encode(String(val ?? "")),
      };
    }
  }
}

/**
 * Extract raw VectorBytes Uint8Array from EntityInput based on collection column type.
 */
function extractVectorBytes(
  collectionDef: Collection | Record<string, unknown>,
  entity: EntityInput,
): Uint8Array {
  const vb = (entity.vectorBytes ?? entity.vector_bytes) as unknown;
  if (vb) {
    if (typeof vb === "string") {
      return Uint8Array.from(Buffer.from(vb, "base64"));
    }
    if (vb instanceof Uint8Array) {
      return vb;
    }
  }

  const v = entity.vector;
  if (v) {
    if (
      v instanceof HalfVector ||
      v instanceof Vector ||
      v instanceof Bit ||
      v instanceof SparseVector
    ) {
      return v.toVectorBytes();
    }
    const rawCol = ((collectionDef as Record<string, unknown>).column ??
      {}) as Record<string, unknown>;
    const colType = String(rawCol.type ?? "halfvec");
    const dims = Number(rawCol.dimensions ?? 0);

    if (colType === "vector") {
      if (typeof v === "string") return Vector.fromText(v).toVectorBytes();
      return new Vector(
        v as unknown as number[] | Float32Array,
      ).toVectorBytes();
    } else if (colType === "halfvec") {
      if (typeof v === "string") return HalfVector.fromText(v).toVectorBytes();
      return new HalfVector(
        v as unknown as number[] | Float32Array,
      ).toVectorBytes();
    } else if (colType === "bit") {
      if (typeof v === "string") return Bit.fromText(v).toVectorBytes();
      return new Bit(v as unknown as Uint8Array, dims).toVectorBytes();
    } else if (colType === "sparsevec") {
      if (typeof v === "string")
        return SparseVector.fromText(v).toVectorBytes();
      const sv = v as unknown as {
        dim: number;
        indices: number[];
        values: number[];
      };
      return new SparseVector(sv.dim, sv.indices, sv.values).toVectorBytes();
    }
  }

  throw new Error("EntityInput must contain either vector or vectorBytes");
}

/**
 * Build direct on-chain entity attributes per docs/api/collections.md §4.
 * Produces arkiv.schema (0x88), av.vec (0x07), av.id (0x88, if present),
 * av.doc (0x07, if present), and user attributes sorted by name.
 */
export function buildEntityAttributes(
  collectionDef: Collection | Record<string, unknown>,
  entity: EntityInput,
): DirectAttribute[] {
  const raw = collectionDef as Record<string, unknown>;
  const schemaTag = String(raw.schemaTag ?? raw.schema_tag ?? raw.name ?? "");

  if (!schemaTag) {
    throw new Error(
      "Collection definition must contain schemaTag or schema_tag",
    );
  }

  const attrs: DirectAttribute[] = [];

  // 1. arkiv.schema (0x88)
  attrs.push({
    name: "arkiv.schema",
    typeId: 0x88,
    valueBytes: new TextEncoder().encode(schemaTag),
  });

  // 2. av.vec (0x07)
  const vecBytes = extractVectorBytes(collectionDef, entity);
  attrs.push({
    name: "av.vec",
    typeId: 0x07,
    valueBytes: vecBytes,
  });

  // 3. av.id (0x88, if present)
  if (entity.id != null) {
    attrs.push({
      name: "av.id",
      typeId: 0x88,
      valueBytes: new TextEncoder().encode(entity.id),
    });
  }

  // 4. av.doc (0x07, if present)
  if (entity.document != null) {
    attrs.push({
      name: "av.doc",
      typeId: 0x07,
      valueBytes: new TextEncoder().encode(entity.document),
    });
  }

  // 5. User attributes sorted by name
  if (entity.attributes) {
    const colAttrs = (raw.attributes ?? {}) as Record<string, string>;
    const userAttrs: DirectAttribute[] = [];
    for (const [k, v] of Object.entries(entity.attributes)) {
      if (k === "arkiv.schema" || k.startsWith("av.")) {
        continue;
      }
      let type: string | undefined;
      let val: unknown = v;
      if (v instanceof TypedValue) {
        type = v.type;
        val = v.value;
      } else if (v && typeof v === "object" && "type" in v && "value" in v) {
        type = (v as { type: string }).type;
        val = (v as { value: unknown }).value;
      } else {
        type = colAttrs[k];
      }
      const encoded = encodeAttributeValue(type, val);
      userAttrs.push({
        name: k,
        typeId: encoded.typeId,
        valueBytes: encoded.valueBytes,
      });
    }

    userAttrs.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
    attrs.push(...userAttrs);
  }

  return attrs;
}

/**
 * Shape attributes for @arkiv-network/sdk createEntity.
 * Docs: PLAN_SDK.md §7.1 item 5
 */
export function toArkivSdkAttributes(
  collectionDef: Collection | Record<string, unknown>,
  entity: EntityInput,
): Record<string, unknown> {
  const raw = collectionDef as Record<string, unknown>;
  const schemaTag = String(raw.schemaTag ?? raw.schema_tag ?? raw.name ?? "");

  if (!schemaTag) {
    throw new Error(
      "Collection definition must contain schemaTag or schema_tag",
    );
  }

  const vecBytes = extractVectorBytes(collectionDef, entity);

  const result: Record<string, unknown> = {
    "arkiv.schema": schemaTag,
    "av.vec": vecBytes,
  };

  if (entity.id != null) {
    result["av.id"] = entity.id;
  }
  if (entity.document != null) {
    result["av.doc"] = new TextEncoder().encode(entity.document);
  }

  if (entity.attributes) {
    const plain = plainAttributes(entity.attributes);
    for (const [k, v] of Object.entries(plain)) {
      if (k === "arkiv.schema" || k.startsWith("av.")) {
        continue;
      }
      result[k] = v;
    }
  }

  return result;
}
