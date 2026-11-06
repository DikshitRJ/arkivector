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
  parseVersionInfo,
  parseReadiness,
  parseSyncStatus,
  parseCollection,
  serializeCollectionCreate,
  serializeFollowRequest,
} from "./models.js";

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
      (typeof process !== "undefined" ? process.env?.ARKIVECTOR_URL : undefined) ??
      "http://127.0.0.1:8080";
    this.url = rawUrl.replace(/\/+$/, "");

    this.apiKey =
      options?.apiKey ??
      (typeof process !== "undefined" ? process.env?.ARKIVECTOR_API_KEY : undefined);

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

    const raw = await this.transport.get<Record<string, unknown>>("/v1/version");
    const info = parseVersionInfo(raw);

    const major = info.apiVersion.split(".")[0];
    if (major !== "1") {
      throw new ApiError(
        `Unsupported daemon API version: ${info.apiVersion}. SDK requires major version 1.`,
        { code: "unsupported_api_version" }
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
        { code: "not_implemented", retryable: false }
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
      if (err instanceof ApiError && err.status === 503 && err.raw && typeof err.raw === "object") {
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
    const raw = await this.transport.get<{ collections?: Record<string, unknown>[] }>("/v1/sync");
    const list = Array.isArray(raw?.collections) ? raw.collections : [];
    return list.map(parseSyncStatus);
  }

  /**
   * List all collections on this daemon: GET /v1/collections.
   */
  async listCollections(): Promise<Collection[]> {
    await this.checkCapability("collections.read");
    const raw = await this.transport.get<{ collections?: Record<string, unknown>[] }>(
      "/v1/collections"
    );
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
      `/v1/collections/${encodeURIComponent(name)}`
    );
    const collection = parseCollection(raw);
    this.columnTypeCache.set(collection.name, collection.column.type);
    return collection;
  }

  /**
   * Define a collection and start syncing: POST /v1/collections.
   */
  async createCollection(options: CreateCollectionOptions): Promise<Collection> {
    if (options.publishManifest || options.publish_manifest) {
      await this.checkCapability("collections.manifest");
    } else {
      await this.checkCapability("collections.write");
    }

    const body = serializeCollectionCreate(options);
    const raw = await this.transport.post<Record<string, unknown>>("/v1/collections", body);
    const collection = parseCollection(raw);
    this.columnTypeCache.set(collection.name, collection.column.type);
    return collection;
  }

  /**
   * Follow an existing collection: POST /v1/collections:follow.
   */
  async followCollection(options: FollowCollectionOptions): Promise<Collection> {
    await this.checkCapability("collections.follow");
    const body = serializeFollowRequest(options);
    const raw = await this.transport.post<Record<string, unknown>>("/v1/collections:follow", body);
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
}

/**
 * Alias Client = Arkivector.
 */
export const Client = Arkivector;
export type Client = Arkivector;
