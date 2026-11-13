/**
 * Arkivector models: TypedValue, T helper, TTL helper, and API interfaces
 * Docs: docs/api/filters.md §2, docs/api/entities.md §7, docs/api/collections.md, docs/api/operations.md
 */

import type { AnyVector } from "./types.js";
import { vectorbytesDecodeAny } from "./vectorbytes.js";
import type { Filter } from "./filter.js";

/**
 * Supported attribute types for Arkiv entities.
 */
export type AttrType =
  "bool" | "i32" | "u64" | "u256" | "dec" | "str" | "addr" | "key" | "bytes";

/**
 * Canonical typed attribute value.
 */
export class TypedValue<TType extends AttrType = AttrType, TVal = unknown> {
  readonly type: TType;
  readonly value: TVal;

  constructor(type: TType, value: TVal) {
    this.type = type;
    this.value = value;
  }

  asBool(): boolean {
    if (this.type !== "bool") {
      throw new TypeError(`Expected bool attribute, got ${this.type}`);
    }
    return Boolean(this.value);
  }
  as_bool(): boolean {
    return this.asBool();
  }

  asI64(): bigint {
    if (this.type !== "i32" && this.type !== "u64") {
      throw new TypeError(`Expected i32 or u64 attribute, got ${this.type}`);
    }
    return BigInt(this.value as string | number | bigint);
  }
  as_i64(): bigint {
    return this.asI64();
  }

  asNumber(): number {
    if (typeof this.value === "number") {
      return this.value;
    }
    const n = Number(this.value);
    if (Number.isNaN(n)) {
      throw new TypeError(
        `Cannot convert value '${String(this.value)}' to number`,
      );
    }
    return n;
  }
  as_number(): number {
    return this.asNumber();
  }

  asStr(): string {
    return String(this.value);
  }
  as_str(): string {
    return this.asStr();
  }

  asDecString(): string {
    if (this.type !== "dec") {
      throw new TypeError(`Expected dec attribute, got ${this.type}`);
    }
    return String(this.value);
  }
  as_dec_string(): string {
    return this.asDecString();
  }

  asBytes(): Uint8Array {
    if (this.type !== "bytes") {
      throw new TypeError(`Expected bytes attribute, got ${this.type}`);
    }
    if (this.value instanceof Uint8Array) {
      return this.value;
    }
    if (typeof this.value === "string") {
      return Uint8Array.from(Buffer.from(this.value, "base64"));
    }
    throw new TypeError(`Unexpected bytes value type: ${typeof this.value}`);
  }
  as_bytes(): Uint8Array {
    return this.asBytes();
  }

  toJSON(): { type: TType; value: TVal } {
    return { type: this.type, value: this.value };
  }
}

/**
 * Value helpers for constructing TypedValue instances.
 */
export const T = {
  bool(x: boolean): TypedValue<"bool", boolean> {
    return new TypedValue("bool", Boolean(x));
  },

  i32(x: number): TypedValue<"i32", number> {
    const val = Math.trunc(x);
    return new TypedValue("i32", val);
  },

  u64(x: string | bigint | number): TypedValue<"u64", string> {
    return new TypedValue("u64", String(x));
  },

  u256(x: string | bigint): TypedValue<"u256", string> {
    return new TypedValue("u256", String(x));
  },

  dec(x: string | number): TypedValue<"dec", string> {
    return new TypedValue("dec", String(x));
  },

  str(x: string): TypedValue<"str", string> {
    return new TypedValue("str", String(x));
  },

  addr(x: string): TypedValue<"addr", string> {
    return new TypedValue("addr", String(x).toLowerCase());
  },

  key(x: string): TypedValue<"key", string> {
    return new TypedValue("key", String(x).toLowerCase());
  },

  bytes(x: Uint8Array | string): TypedValue<"bytes", string> {
    if (typeof x === "string") {
      return new TypedValue("bytes", x);
    }
    return new TypedValue("bytes", Buffer.from(x).toString("base64"));
  },
};

/**
 * TTL input types per docs/api/entities.md §7.
 * Exactly one field is specified.
 */
export type Ttl =
  | { blocks: number }
  | { seconds: number }
  | { days: number }
  | { expires_at_block: number };

/**
 * TTL input accepting both snake_case and camelCase.
 */
export type TtlInput =
  | Ttl
  | { blocks: number }
  | { seconds: number }
  | { days: number }
  | { expires_at_block: number }
  | { expiresAtBlock: number };

/**
 * TTL helpers for setting entity lifetime.
 */
export const Ttl = {
  blocks(n: number): { blocks: number } {
    if (n < 1 || n > 7884000) {
      throw new RangeError("blocks must be between 1 and 7884000");
    }
    return { blocks: Math.trunc(n) };
  },

  seconds(n: number): { seconds: number } {
    if (n < 2 || n > 15768000 || n % 2 !== 0) {
      throw new RangeError(
        "seconds must be an even integer between 2 and 15768000",
      );
    }
    return { seconds: Math.trunc(n) };
  },

  days(x: number): { days: number } {
    if (x <= 0 || x > 182.5) {
      throw new RangeError("days must be > 0 and <= 182.5");
    }
    return { days: x };
  },

  atBlock(n: number): { expires_at_block: number } {
    return { expires_at_block: Math.trunc(n) };
  },

  at_block(n: number): { expires_at_block: number } {
    return { expires_at_block: Math.trunc(n) };
  },
};

/**
 * Helper to convert map of TypedValue into plain JavaScript scalars.
 * u64, u256, dec, addr, key stay strings; bytes stays Uint8Array.
 */
export function plainAttributes(
  attrs: Record<string, TypedValue | unknown>,
): Record<string, unknown> {
  const result: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(attrs)) {
    if (v instanceof TypedValue) {
      if (v.type === "bool") {
        result[k] = v.asBool();
      } else if (v.type === "i32") {
        result[k] = v.asNumber();
      } else if (
        v.type === "dec" ||
        v.type === "u64" ||
        v.type === "u256" ||
        v.type === "str" ||
        v.type === "addr" ||
        v.type === "key"
      ) {
        result[k] = v.asStr();
      } else if (v.type === "bytes") {
        result[k] = v.asBytes();
      } else {
        result[k] = v.value;
      }
    } else {
      result[k] = v;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Operations and Collections Models
// ---------------------------------------------------------------------------

/**
 * Daemon version, API version and capabilities returned by GET /v1/version.
 */
export interface VersionInfo {
  daemonVersion: string;
  apiVersion: string;
  pgvectorVersion: string;
  kernelTier: string;
  chainId: number;
  writesEnabled: boolean;
  capabilities: string[];
  /** Managed signer kind and address (contract 1.1.0); null without one. Never key material. */
  signer: SignerInfo | null;
}

/**
 * The daemon's managed signer, from GET /v1/version (contract 1.1.0).
 */
export interface SignerInfo {
  kind: "env" | "keystore" | "remote" | string;
  address: string;
}

/**
 * Readiness probe response from GET /readyz.
 */
export interface Readiness {
  ready: boolean;
  collections: Record<string, string>;
}

/**
 * Sync status of a followed collection.
 */
export interface SyncStatus {
  collection: string;
  state: string;
  headBlock: number;
  asOfBlock: number;
  indexedBlock?: number;
  chainId?: number;
  schemaTag?: string;
  lastSyncTime?: string | number | null;
  quarantineCount?: number;
  error?: string | null;
  // Aliases for wire field compatibility
  name?: string;
  lagBlocks?: number;
  lastError?: string | null;
}

/**
 * Column definition for vector table.
 */
export type VectorColumnType = "vector" | "halfvec" | "bit" | "sparsevec";

export interface ColumnDef {
  type: VectorColumnType | string;
  dimensions: number;
}

export type Column = ColumnDef;

/**
 * Index definition.
 */
export interface IndexDef {
  method: "hnsw" | "none" | string;
  opclass: string;
  with?: Record<string, unknown>;
}

/**
 * Collection statistics.
 */
export interface CollectionStats {
  liveEntities: number;
  segments: number;
  quarantined: number;
  tombstones: number;
  [key: string]: unknown;
}

/**
 * Collection model representing a synced pgvector collection.
 */
export interface Collection {
  name: string;
  schemaTag: string;
  column: ColumnDef;
  index: IndexDef;
  writers: string[];
  membership: "creator" | "owner" | "open" | string;
  attributes: Record<string, string>;
  defaultTtl?: TtlInput | null;
  open: boolean;
  manifestKey?: string | null;
  stats?: CollectionStats | Record<string, unknown>;
  sync?: SyncStatus;
  model?: string | null;
  mode?: "full" | "lite" | string;
}

/**
 * Options for creating a collection via POST /v1/collections.
 */
export interface CreateCollectionOptions {
  name: string;
  column: ColumnDef;
  index: IndexDef;
  schemaTag?: string;
  schema_tag?: string;
  writers?: string[];
  membership?: "creator" | "owner" | "open" | string;
  attributes?: Record<string, AttrType | string>;
  defaultTtl?: TtlInput | null;
  default_ttl?: TtlInput | null;
  publishManifest?: boolean;
  publish_manifest?: boolean;
  model?: string | null;
  mode?: "full" | "lite" | string;
}

/**
 * Options for following an existing collection via POST /v1/collections:follow.
 */
export interface FollowCollectionOptions {
  manifestKey?: string;
  manifest_key?: string;
  name?: string;
  definition?: CreateCollectionOptions;
}

/**
 * Client configuration options.
 */
export interface ClientOptions {
  url?: string;
  apiKey?: string;
  timeoutMs?: number;
  timeout?: number;
  maxRetries?: number;
  sessionConsistency?: boolean;
  idempotency?: boolean;
  fetch?: typeof globalThis.fetch;
  userAgent?: string;
}

// ---------------------------------------------------------------------------
// Model Parsers & Serializers
// ---------------------------------------------------------------------------

/**
 * Parse wire JSON into VersionInfo.
 */
export function parseVersionInfo(raw: Record<string, unknown>): VersionInfo {
  return {
    daemonVersion: String(raw.daemon_version ?? raw.daemonVersion ?? ""),
    apiVersion: String(raw.api_version ?? raw.apiVersion ?? ""),
    pgvectorVersion: String(raw.pgvector_version ?? raw.pgvectorVersion ?? ""),
    kernelTier: String(raw.kernel_tier ?? raw.kernelTier ?? ""),
    chainId: Number(raw.chain_id ?? raw.chainId ?? 0),
    writesEnabled: Boolean(raw.writes_enabled ?? raw.writesEnabled ?? false),
    capabilities: Array.isArray(raw.capabilities)
      ? raw.capabilities.map(String)
      : [],
    signer:
      raw.signer != null && typeof raw.signer === "object"
        ? {
            kind: String((raw.signer as Record<string, unknown>).kind ?? ""),
            address: String(
              (raw.signer as Record<string, unknown>).address ?? "",
            ),
          }
        : null,
  };
}

/**
 * Parse wire JSON into Readiness.
 */
export function parseReadiness(raw: Record<string, unknown>): Readiness {
  return {
    ready: Boolean(raw.ready ?? false),
    collections: (raw.collections ?? {}) as Record<string, string>,
  };
}

/**
 * Parse wire JSON into SyncStatus.
 */
export function parseSyncStatus(raw: Record<string, unknown>): SyncStatus {
  const collection = String(raw.collection ?? raw.name ?? "");
  const state = String(raw.state ?? "");
  const asOfBlock =
    typeof raw.as_of_block === "number"
      ? raw.as_of_block
      : typeof raw.asOfBlock === "number"
        ? raw.asOfBlock
        : 0;
  const headBlock =
    typeof raw.head_block === "number"
      ? raw.head_block
      : typeof raw.headBlock === "number"
        ? raw.headBlock
        : 0;
  const indexedBlock =
    typeof raw.indexed_block === "number"
      ? raw.indexed_block
      : typeof raw.indexedBlock === "number"
        ? raw.indexedBlock
        : asOfBlock;
  const lagBlocks =
    typeof raw.lag_blocks === "number"
      ? raw.lag_blocks
      : typeof raw.lagBlocks === "number"
        ? raw.lagBlocks
        : headBlock >= asOfBlock
          ? headBlock - asOfBlock
          : 0;
  const errVal = raw.error ?? raw.last_error ?? raw.lastError;
  const error = errVal != null ? String(errVal) : null;

  return {
    collection,
    name: collection,
    state,
    asOfBlock,
    headBlock,
    indexedBlock,
    lagBlocks,
    error,
    lastError: error,
    chainId:
      typeof raw.chain_id === "number"
        ? raw.chain_id
        : typeof raw.chainId === "number"
          ? raw.chainId
          : undefined,
    schemaTag:
      typeof raw.schema_tag === "string"
        ? raw.schema_tag
        : typeof raw.schemaTag === "string"
          ? raw.schemaTag
          : undefined,
    lastSyncTime: (raw.last_sync_time ?? raw.lastSyncTime) as
      string | number | null | undefined,
    quarantineCount:
      typeof raw.quarantine_count === "number"
        ? raw.quarantine_count
        : typeof raw.quarantineCount === "number"
          ? raw.quarantineCount
          : undefined,
  };
}

/**
 * Parse wire JSON into Collection.
 */
export function parseCollection(raw: Record<string, unknown>): Collection {
  const name = String(raw.name ?? "");
  const schemaTag = String(raw.schema_tag ?? raw.schemaTag ?? "");
  const rawCol = (raw.column ?? {}) as Record<string, unknown>;
  const column: ColumnDef = {
    type: String(rawCol.type ?? "halfvec"),
    dimensions: Number(rawCol.dimensions ?? 0),
  };
  const rawIdx = (raw.index ?? {}) as Record<string, unknown>;
  const index: IndexDef = {
    method: String(rawIdx.method ?? "hnsw"),
    opclass: String(rawIdx.opclass ?? ""),
  };
  if (rawIdx.with && typeof rawIdx.with === "object") {
    index.with = { ...(rawIdx.with as Record<string, unknown>) };
  }
  const writers = Array.isArray(raw.writers) ? raw.writers.map(String) : [];
  const membership = String(raw.membership ?? "creator");
  const attributes = (raw.attributes ?? {}) as Record<string, string>;
  const defaultTtl = (raw.default_ttl ?? raw.defaultTtl) as
    TtlInput | null | undefined;
  const open = Boolean(raw.open ?? membership === "open");
  const manifestKey = (raw.manifest_key ?? raw.manifestKey) as
    string | null | undefined;

  let stats: CollectionStats | undefined = undefined;
  if (raw.stats && typeof raw.stats === "object") {
    const s = raw.stats as Record<string, unknown>;
    stats = {
      liveEntities: Number(s.live_entities ?? s.liveEntities ?? 0),
      segments: Number(s.segments ?? 0),
      quarantined: Number(s.quarantined ?? 0),
      tombstones: Number(s.tombstones ?? 0),
      ...s,
    };
  }

  let sync: SyncStatus | undefined = undefined;
  if (raw.sync && typeof raw.sync === "object") {
    sync = parseSyncStatus(raw.sync as Record<string, unknown>);
  }

  return {
    name,
    schemaTag,
    column,
    index,
    writers,
    membership,
    attributes,
    defaultTtl: defaultTtl ?? null,
    open,
    manifestKey: manifestKey ?? null,
    stats,
    sync,
    model: (raw.model as string | null) ?? null,
    mode: raw.mode != null ? String(raw.mode) : undefined,
  };
}

/**
 * Serialize TtlInput into wire JSON format.
 */
export function serializeTtl(
  ttl?: TtlInput | null,
): Record<string, unknown> | null | undefined {
  if (ttl === undefined) return undefined;
  if (ttl === null) return null;
  if ("blocks" in ttl) return { blocks: ttl.blocks };
  if ("seconds" in ttl) return { seconds: ttl.seconds };
  if ("days" in ttl) return { days: ttl.days };
  if ("expires_at_block" in ttl)
    return { expires_at_block: ttl.expires_at_block };
  if ("expiresAtBlock" in ttl) return { expires_at_block: ttl.expiresAtBlock };
  return ttl as Record<string, unknown>;
}

/**
 * Serialize CreateCollectionOptions into wire JSON CollectionCreate format.
 */
export function serializeCollectionCreate(
  opts: CreateCollectionOptions,
): Record<string, unknown> {
  const result: Record<string, unknown> = {
    name: opts.name,
    column: {
      type: opts.column.type,
      dimensions: opts.column.dimensions,
    },
    index: {
      method: opts.index.method,
      opclass: opts.index.opclass,
    },
  };

  if (opts.index.with) {
    const w: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(opts.index.with)) {
      if (k === "efConstruction" || k === "ef_construction") {
        w.ef_construction = v;
      } else {
        w[k] = v;
      }
    }
    (result.index as Record<string, unknown>).with = w;
  }

  const schemaTag = opts.schemaTag ?? opts.schema_tag;
  if (schemaTag !== undefined) {
    result.schema_tag = schemaTag;
  }
  if (opts.writers !== undefined) {
    result.writers = opts.writers;
  }
  if (opts.membership !== undefined) {
    result.membership = opts.membership;
  }
  if (opts.attributes !== undefined) {
    result.attributes = opts.attributes;
  }
  const defaultTtl = opts.defaultTtl ?? opts.default_ttl;
  if (defaultTtl !== undefined) {
    result.default_ttl = serializeTtl(defaultTtl);
  }
  const publishManifest = opts.publishManifest ?? opts.publish_manifest;
  if (publishManifest !== undefined) {
    result.publish_manifest = publishManifest;
  }
  if (opts.model !== undefined) {
    result.model = opts.model;
  }
  if (opts.mode !== undefined) {
    result.mode = opts.mode;
  }

  return result;
}

/**
 * Serialize FollowCollectionOptions into wire JSON FollowRequest format.
 */
export function serializeFollowRequest(
  opts: FollowCollectionOptions,
): Record<string, unknown> {
  const manifestKey = opts.manifestKey ?? opts.manifest_key;
  if (manifestKey !== undefined) {
    const res: Record<string, unknown> = { manifest_key: manifestKey };
    if (opts.name !== undefined) {
      res.name = opts.name;
    }
    return res;
  }
  if (opts.definition !== undefined) {
    return {
      definition: serializeCollectionCreate(opts.definition),
    };
  }
  throw new Error(
    "FollowCollectionOptions must provide either manifestKey or definition",
  );
}

// ---------------------------------------------------------------------------
// Search, Consistency, and Read Models
// ---------------------------------------------------------------------------

/**
 * Consistency specification for read requests.
 * Docs: docs/api/consistency.md
 */
export interface Consistency {
  minBlock?: number;
  min_block?: number;
  timeoutMs?: number;
  timeout_ms?: number;
}

/**
 * Serialize Consistency into wire JSON format.
 */
export function serializeConsistency(
  c?: Consistency,
): { min_block?: number; timeout_ms?: number } | undefined {
  if (!c) return undefined;
  const min_block = c.minBlock ?? c.min_block;
  const timeout_ms = c.timeoutMs ?? c.timeout_ms;
  if (min_block === undefined && timeout_ms === undefined) return undefined;
  const result: { min_block?: number; timeout_ms?: number } = {};
  if (min_block !== undefined) result.min_block = min_block;
  if (timeout_ms !== undefined) result.timeout_ms = timeout_ms;
  return result;
}

/**
 * Parse an attributes map into typed values.
 */
export function parseAttributes(
  raw: Record<string, unknown> | undefined | null,
): Record<string, TypedValue> {
  const result: Record<string, TypedValue> = {};
  if (!raw || typeof raw !== "object") return result;

  for (const [k, v] of Object.entries(raw)) {
    if (v instanceof TypedValue) {
      result[k] = v;
    } else if (v && typeof v === "object" && "type" in v && "value" in v) {
      const obj = v as { type: AttrType; value: unknown };
      result[k] = new TypedValue(obj.type, obj.value);
    } else if (typeof v === "string") {
      result[k] = T.str(v);
    } else if (typeof v === "boolean") {
      result[k] = T.bool(v);
    } else if (typeof v === "number") {
      if (Number.isInteger(v)) {
        result[k] = T.i32(v);
      } else {
        result[k] = T.dec(String(v));
      }
    } else if (typeof v === "bigint") {
      result[k] = T.u64(String(v));
    } else {
      result[k] = new TypedValue("str", String(v));
    }
  }
  return result;
}

/**
 * Entity model representing one Arkiv entity in a collection.
 * Docs: docs/api/entities.md §1
 */
export class Entity {
  readonly key: string;
  readonly id: string | null;
  readonly owner: string;
  readonly creator: string;
  readonly createdAtBlock: number;
  readonly updatedAtBlock: number;
  readonly expiresAtBlock: number;
  readonly attributes: Record<string, TypedValue>;
  readonly document: string | null;
  readonly vectorBytes: string | null;
  vector: AnyVector | null;

  get created_at_block(): number {
    return this.createdAtBlock;
  }
  get updated_at_block(): number {
    return this.updatedAtBlock;
  }
  get expires_at_block(): number {
    return this.expiresAtBlock;
  }
  get vector_bytes(): string | null {
    return this.vectorBytes;
  }

  constructor(init: {
    key: string;
    id?: string | null;
    owner: string;
    creator: string;
    createdAtBlock: number;
    updatedAtBlock: number;
    expiresAtBlock: number;
    attributes: Record<string, TypedValue>;
    document?: string | null;
    vectorBytes?: string | null;
    vector?: AnyVector | null;
  }) {
    this.key = init.key;
    this.id = init.id ?? null;
    this.owner = init.owner;
    this.creator = init.creator;
    this.createdAtBlock = init.createdAtBlock;
    this.updatedAtBlock = init.updatedAtBlock;
    this.expiresAtBlock = init.expiresAtBlock;
    this.attributes = init.attributes;
    this.document = init.document ?? null;
    this.vectorBytes = init.vectorBytes ?? null;
    this.vector = init.vector ?? null;
  }

  plainAttributes(): Record<string, unknown> {
    return plainAttributes(this.attributes);
  }
}

/**
 * Search hit item returned in SearchResult.
 * Docs: docs/api/search.md §1
 */
export class SearchHit {
  readonly key: string;
  readonly id: string | null;
  readonly distance: number;
  readonly score: number;
  readonly owner: string;
  readonly creator: string;
  readonly expiresAtBlock: number;
  readonly attributes: Record<string, TypedValue>;
  readonly document: string | null;
  readonly vectorBytes: string | null;
  vector: AnyVector | null;

  get expires_at_block(): number {
    return this.expiresAtBlock;
  }
  get vector_bytes(): string | null {
    return this.vectorBytes;
  }

  constructor(init: {
    key: string;
    id?: string | null;
    distance: number;
    score: number;
    owner: string;
    creator: string;
    expiresAtBlock: number;
    attributes: Record<string, TypedValue>;
    document?: string | null;
    vectorBytes?: string | null;
    vector?: AnyVector | null;
  }) {
    this.key = init.key;
    this.id = init.id ?? null;
    this.distance = init.distance;
    this.score = init.score;
    this.owner = init.owner;
    this.creator = init.creator;
    this.expiresAtBlock = init.expiresAtBlock;
    this.attributes = init.attributes;
    this.document = init.document ?? null;
    this.vectorBytes = init.vectorBytes ?? null;
    this.vector = init.vector ?? null;
  }

  plainAttributes(): Record<string, unknown> {
    return plainAttributes(this.attributes);
  }
}

/**
 * Search result object from search().
 * Docs: docs/api/search.md §1
 */
export class SearchResult {
  readonly asOfBlock: number;
  readonly syncState: string;
  readonly openCollection: boolean;
  readonly hits: SearchHit[];
  readonly plan?: Record<string, unknown>;

  get as_of_block(): number {
    return this.asOfBlock;
  }
  get sync_state(): string {
    return this.syncState;
  }
  get open_collection(): boolean {
    return this.openCollection;
  }
  get results(): SearchHit[] {
    return this.hits;
  }

  constructor(init: {
    asOfBlock: number;
    syncState: string;
    openCollection?: boolean;
    hits: SearchHit[];
    plan?: Record<string, unknown>;
  }) {
    this.asOfBlock = init.asOfBlock;
    this.syncState = init.syncState;
    this.openCollection = init.openCollection ?? false;
    this.hits = init.hits;
    this.plan = init.plan;
  }
}

/**
 * Result of batch search from searchBatch().
 * Docs: docs/api/search.md §2
 */
export class BatchResult {
  readonly asOfBlock: number;
  readonly syncState: string;
  readonly results: SearchResult[];

  get as_of_block(): number {
    return this.asOfBlock;
  }
  get sync_state(): string {
    return this.syncState;
  }
  get responses(): SearchResult[] {
    return this.results;
  }

  constructor(init: {
    asOfBlock: number;
    syncState: string;
    results: SearchResult[];
  }) {
    this.asOfBlock = init.asOfBlock;
    this.syncState = init.syncState;
    this.results = init.results;
  }
}

/**
 * One page of scroll results.
 * Docs: docs/api/search.md §3
 */
export class ScrollPage {
  readonly entities: Entity[];
  readonly nextCursor: string | null;
  readonly asOfBlock: number;

  get next_cursor(): string | null {
    return this.nextCursor;
  }
  get as_of_block(): number {
    return this.asOfBlock;
  }

  constructor(init: {
    entities: Entity[];
    nextCursor?: string | null;
    asOfBlock: number;
  }) {
    this.entities = init.entities;
    this.nextCursor = init.nextCursor ?? null;
    this.asOfBlock = init.asOfBlock;
  }
}

/**
 * Result of count().
 * Docs: docs/api/search.md §4
 */
export class CountResult {
  readonly count: number;
  readonly asOfBlock: number;

  get as_of_block(): number {
    return this.asOfBlock;
  }

  constructor(init: { count: number; asOfBlock: number }) {
    this.count = init.count;
    this.asOfBlock = init.asOfBlock;
  }
}

/**
 * Options for search().
 */
export interface SearchOptions {
  vector?:
    | AnyVector
    | number[]
    | Float32Array
    | Float64Array
    | string
    | { dim: number; indices: number[]; values: number[] }
    | unknown;
  vectorBytes?: string | Uint8Array;
  vector_bytes?: string | Uint8Array;
  k?: number;
  offset?: number;
  operator?: "<->" | "<#>" | "<=>" | "<+>" | "<~>" | "<%>" | string;
  filter?: Filter;
  where?: string;
  efSearch?: number;
  ef_search?: number;
  iterativeScan?: "off" | "relaxed_order" | "strict_order" | string;
  iterative_scan?: "off" | "relaxed_order" | "strict_order" | string;
  maxScanTuples?: number;
  max_scan_tuples?: number;
  maxDistance?: number;
  max_distance?: number;
  minScore?: number;
  min_score?: number;
  exact?: boolean;
  include?: ("attributes" | "document" | "vector" | string)[];
  consistency?: Consistency;
  explain?: boolean;
}

/**
 * Options for scroll() and scrollPage().
 */
export interface ScrollOptions {
  filter?: Filter;
  where?: string;
  limit?: number;
  cursor?: string | null;
  include?: ("attributes" | "document" | "vector" | string)[];
  consistency?: Consistency;
}

/**
 * Options for count().
 */
export interface CountOptions {
  filter?: Filter;
  where?: string;
  consistency?: Consistency;
}

/**
 * Options for get().
 */
export interface GetEntityOptions {
  include?: ("attributes" | "document" | "vector" | string)[];
  verify?: boolean;
  consistency?: Consistency;
  minBlock?: number;
  min_block?: number;
  timeoutMs?: number;
  timeout_ms?: number;
}

/**
 * Direct-write on-chain attribute representation.
 * Docs: docs/api/collections.md §4
 */
export interface DirectAttribute {
  name: string;
  typeId: number;
  valueBytes: Uint8Array;
}

/**
 * Input representation of an entity for managed upsert or prepare.
 * Docs: docs/api/entities.md §3, §4
 */
export interface EntityInput {
  id?: string | null;
  vector?:
    | AnyVector
    | number[]
    | Float32Array
    | Float64Array
    | string
    | Record<string, unknown>;
  vectorBytes?: string;
  vector_bytes?: string;
  document?: string | null;
  attributes?: Record<string, TypedValue | unknown>;
  ttl?: TtlInput;
}

/**
 * Result for a single entity operation within an upsert / prepare call.
 * Docs: docs/api/entities.md §3, §4
 */
export interface UpsertItemResult {
  id?: string | null;
  key: string;
  op: "create" | "patch" | "noop" | string;
}

/**
 * Result of a managed upsert operation.
 * Docs: docs/api/entities.md §3
 */
export class UpsertResult {
  readonly results: UpsertItemResult[];
  readonly txHashes: string[];
  readonly blockNumber?: number | null;

  get tx_hashes(): string[] {
    return this.txHashes;
  }
  get block_number(): number | null | undefined {
    return this.blockNumber;
  }

  constructor(init: {
    results: UpsertItemResult[];
    txHashes: string[];
    blockNumber?: number | null;
  }) {
    this.results = init.results;
    this.txHashes = init.txHashes;
    this.blockNumber = init.blockNumber ?? null;
  }
}

/**
 * Unsigned EIP-1559 transaction for client-side signing (plain JSON: safe to send to a browser).
 * Since contract 1.1.0 it carries everything needed to sign it as is. Convert it with
 * toRpcTransaction (EIP-1193 wallets such as MetaMask), toViemTransaction or toEthersTransaction.
 * Docs: docs/api/entities.md §4
 */
export interface PreparedTransaction {
  /** Always 2 (EIP-1559). */
  type: number;
  chainId: number | null;
  from: string | null;
  to: string;
  data: string;
  value: string;
  nonce: number;
  gasEstimate: number | null;
  gas_estimate?: number | null;
  /** Gas limit to sign with (eth_estimateGas + margin). */
  gas: number | null;
  /** Fee suggestion in wei as 0x-hex; replace it before signing if you like. */
  maxFeePerGas: string | null;
  maxPriorityFeePerGas: string | null;
}

/**
 * Fee suggestion returned by entities:prepare (wei as 0x-hex).
 */
export interface FeeSuggestion {
  maxFeePerGas: string;
  maxPriorityFeePerGas: string;
  source: "fee_history" | "gas_price" | "static" | string;
}

/**
 * Result of prepareEntities.
 * Docs: docs/api/entities.md §4
 */
export class PrepareResult {
  readonly chainId: number;
  readonly transactions: PreparedTransaction[];
  readonly results: UpsertItemResult[];
  readonly from: string | null;
  readonly fees: FeeSuggestion | null;

  get chain_id(): number {
    return this.chainId;
  }

  constructor(init: {
    chainId: number;
    transactions: PreparedTransaction[];
    results: UpsertItemResult[];
    from?: string | null;
    fees?: FeeSuggestion | null;
  }) {
    this.chainId = init.chainId;
    this.transactions = init.transactions;
    this.results = init.results;
    this.from = init.from ?? null;
    this.fees = init.fees ?? null;
  }
}

/**
 * Result of submit(): POST /v1/collections/{name}/entities:submit (contract 1.1.0).
 * Docs: docs/api/entities.md §4.2
 */
export class SubmitResult {
  readonly txHashes: string[];
  readonly blockNumber: number | null;
  /** Keys of the EntityCreated logs of the receipts, in order (empty for wait "none"). */
  readonly createdKeys: string[];

  get tx_hashes(): string[] {
    return this.txHashes;
  }
  get block_number(): number | null {
    return this.blockNumber;
  }
  get created_keys(): string[] {
    return this.createdKeys;
  }

  constructor(init: {
    txHashes: string[];
    blockNumber?: number | null;
    createdKeys?: string[];
  }) {
    this.txHashes = init.txHashes;
    this.blockNumber = init.blockNumber ?? null;
    this.createdKeys = init.createdKeys ?? [];
  }
}

/**
 * Parse wire JSON into SubmitResult.
 */
export function parseSubmitResult(raw: Record<string, unknown>): SubmitResult {
  const bn = raw.block_number ?? raw.blockNumber;
  return new SubmitResult({
    txHashes: Array.isArray(raw.tx_hashes) ? raw.tx_hashes.map(String) : [],
    blockNumber: typeof bn === "number" ? bn : null,
    createdKeys: Array.isArray(raw.created_keys)
      ? raw.created_keys.map(String)
      : [],
  });
}

/**
 * Result of a delete or extend operation.
 * Docs: docs/api/entities.md §5
 */
export class TxResult {
  readonly affected: number;
  readonly txHashes: string[];
  readonly blockNumber?: number | null;

  get tx_hashes(): string[] {
    return this.txHashes;
  }
  get block_number(): number | null | undefined {
    return this.blockNumber;
  }

  constructor(init: {
    affected: number;
    txHashes: string[];
    blockNumber?: number | null;
  }) {
    this.affected = init.affected;
    this.txHashes = init.txHashes;
    this.blockNumber = init.blockNumber ?? null;
  }
}

/**
 * Options for upsert().
 */
export interface UpsertOptions {
  wait?: "none" | "receipt" | "indexed" | string;
  timeoutMs?: number;
  timeout_ms?: number;
  idempotencyKey?: string;
  idempotency_key?: string;
  idempotency?: boolean;
}

/**
 * Options for prepare().
 */
export interface PrepareOptions {
  from: string;
}

/**
 * Options for submit(): exactly one of signedTransactions or txHashes.
 */
export interface SubmitOptions {
  /** Raw signed EIP-1559 transactions (0x02… hex or bytes); the daemon broadcasts them in order. */
  signedTransactions?: (string | Uint8Array)[];
  /** Hashes of transactions a wallet already broadcast (e.g. MetaMask eth_sendTransaction). */
  txHashes?: string[];
  wait?: "none" | "receipt" | "indexed" | string;
  timeoutMs?: number;
  idempotencyKey?: string;
  idempotency?: boolean;
}

/**
 * Options for delete().
 */
export interface DeleteEntitiesOptions {
  ref?: string;
  refs?: string[];
  filter?: Filter;
  where?: string;
  idempotencyKey?: string;
  idempotency_key?: string;
  idempotency?: boolean;
  timeoutMs?: number;
  timeout_ms?: number;
}

/**
 * Options for extend().
 */
export interface ExtendEntitiesOptions {
  refs?: string[];
  filter?: Filter;
  where?: string;
  idempotencyKey?: string;
  idempotency_key?: string;
  idempotency?: boolean;
  timeoutMs?: number;
  timeout_ms?: number;
}

/**
 * Parse wire JSON into Entity.
 */
export function parseEntity(raw: Record<string, unknown>): Entity {
  const key = String(raw.key ?? "");
  const id = raw.id != null ? String(raw.id) : null;
  const owner = String(raw.owner ?? "");
  const creator = String(raw.creator ?? "");
  const createdAtBlock = Number(
    raw.created_at_block ?? raw.createdAtBlock ?? 0,
  );
  const updatedAtBlock = Number(
    raw.updated_at_block ?? raw.updatedAtBlock ?? 0,
  );
  const expiresAtBlock = Number(
    raw.expires_at_block ?? raw.expiresAtBlock ?? 0,
  );
  const attributes = parseAttributes(
    (raw.attributes as Record<string, unknown>) ?? {},
  );
  const document = raw.document != null ? String(raw.document) : null;
  const vb = (raw.vector_bytes ?? raw.vectorBytes) as string | null | undefined;
  const vectorBytes = vb != null ? String(vb) : null;

  let vector: AnyVector | null = null;
  if (vectorBytes) {
    try {
      const buf = Buffer.from(vectorBytes, "base64");
      if (buf.length >= 4) {
        vector = vectorbytesDecodeAny(
          new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength),
        );
      }
    } catch {
      vector = null;
    }
  }

  return new Entity({
    key,
    id,
    owner,
    creator,
    createdAtBlock,
    updatedAtBlock,
    expiresAtBlock,
    attributes,
    document,
    vectorBytes,
    vector,
  });
}

/**
 * Parse wire JSON into SearchHit.
 */
export function parseSearchHit(raw: Record<string, unknown>): SearchHit {
  const key = String(raw.key ?? "");
  const id = raw.id != null ? String(raw.id) : null;
  const distance = Number(raw.distance ?? 0);
  const score = Number(raw.score ?? 0);
  const owner = String(raw.owner ?? "");
  const creator = String(raw.creator ?? "");
  const expiresAtBlock = Number(
    raw.expires_at_block ?? raw.expiresAtBlock ?? 0,
  );
  const attributes = parseAttributes(
    (raw.attributes as Record<string, unknown>) ?? {},
  );
  const document = raw.document != null ? String(raw.document) : null;
  const vb = (raw.vector_bytes ?? raw.vectorBytes) as string | null | undefined;
  const vectorBytes = vb != null ? String(vb) : null;

  let vector: AnyVector | null = null;
  if (vectorBytes) {
    try {
      const buf = Buffer.from(vectorBytes, "base64");
      if (buf.length >= 4) {
        vector = vectorbytesDecodeAny(
          new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength),
        );
      }
    } catch {
      vector = null;
    }
  }

  return new SearchHit({
    key,
    id,
    distance,
    score,
    owner,
    creator,
    expiresAtBlock,
    attributes,
    document,
    vectorBytes,
    vector,
  });
}

/**
 * Parse wire JSON into SearchResult.
 */
export function parseSearchResult(raw: Record<string, unknown>): SearchResult {
  const asOfBlock = Number(raw.as_of_block ?? raw.asOfBlock ?? 0);
  const syncState = String(raw.sync_state ?? raw.syncState ?? "");
  const openCollection = Boolean(
    raw.open_collection ?? raw.openCollection ?? false,
  );
  const rawHits = Array.isArray(raw.results)
    ? raw.results
    : Array.isArray(raw.hits)
      ? raw.hits
      : [];
  const hits = rawHits.map((h) => parseSearchHit(h as Record<string, unknown>));
  const plan = (raw.plan as Record<string, unknown> | undefined) ?? undefined;

  return new SearchResult({
    asOfBlock,
    syncState,
    openCollection,
    hits,
    plan,
  });
}

/**
 * Parse wire JSON into BatchResult.
 */
export function parseBatchResult(raw: Record<string, unknown>): BatchResult {
  const asOfBlock = Number(raw.as_of_block ?? raw.asOfBlock ?? 0);
  const syncState = String(raw.sync_state ?? raw.syncState ?? "");
  const rawResponses = Array.isArray(raw.responses) ? raw.responses : [];
  const results = rawResponses.map((r: Record<string, unknown>) => {
    const rawHits = Array.isArray(r.results) ? r.results : [];
    const hits = rawHits.map((h) =>
      parseSearchHit(h as Record<string, unknown>),
    );
    const plan = (r.plan as Record<string, unknown> | undefined) ?? undefined;
    return new SearchResult({
      asOfBlock,
      syncState,
      openCollection: false,
      hits,
      plan,
    });
  });

  return new BatchResult({
    asOfBlock,
    syncState,
    results,
  });
}

/**
 * Parse wire JSON into ScrollPage.
 */
export function parseScrollPage(raw: Record<string, unknown>): ScrollPage {
  const asOfBlock = Number(raw.as_of_block ?? raw.asOfBlock ?? 0);
  const rawEntities = Array.isArray(raw.entities) ? raw.entities : [];
  const entities = rawEntities.map((e) =>
    parseEntity(e as Record<string, unknown>),
  );
  const nextCursor =
    raw.next_cursor != null
      ? String(raw.next_cursor)
      : raw.nextCursor != null
        ? String(raw.nextCursor)
        : null;

  return new ScrollPage({
    entities,
    nextCursor,
    asOfBlock,
  });
}

/**
 * Parse wire JSON into CountResult.
 */
export function parseCountResult(raw: Record<string, unknown>): CountResult {
  return new CountResult({
    count: Number(raw.count ?? 0),
    asOfBlock: Number(raw.as_of_block ?? raw.asOfBlock ?? 0),
  });
}

/**
 * Parse wire JSON into UpsertResult.
 */
export function parseUpsertResult(raw: Record<string, unknown>): UpsertResult {
  const rawResults = Array.isArray(raw.results) ? raw.results : [];
  const results: UpsertItemResult[] = rawResults.map(
    (r: Record<string, unknown>) => ({
      id: r.id != null ? String(r.id) : null,
      key: String(r.key ?? ""),
      op: (r.op ?? "create") as "create" | "patch" | "noop",
    }),
  );
  const rawHashes = Array.isArray(raw.tx_hashes)
    ? raw.tx_hashes
    : Array.isArray(raw.txHashes)
      ? raw.txHashes
      : [];
  const txHashes = rawHashes.map(String);
  const blockNumber =
    typeof raw.block_number === "number"
      ? raw.block_number
      : typeof raw.blockNumber === "number"
        ? raw.blockNumber
        : null;

  return new UpsertResult({ results, txHashes, blockNumber });
}

/**
 * Parse wire JSON into PrepareResult.
 */
export function parsePrepareResult(
  raw: Record<string, unknown>,
): PrepareResult {
  const chainId = Number(raw.chain_id ?? raw.chainId ?? 0);
  const rawTxs = Array.isArray(raw.transactions) ? raw.transactions : [];
  const transactions: PreparedTransaction[] = rawTxs.map(
    (t: Record<string, unknown>) => ({
      type: Number(t.type ?? 2),
      chainId:
        t.chain_id != null
          ? Number(t.chain_id)
          : t.chainId != null
            ? Number(t.chainId)
            : null,
      from: t.from != null ? String(t.from) : null,
      gas:
        t.gas != null
          ? Number(t.gas)
          : t.gas_estimate != null
            ? Number(t.gas_estimate)
            : null,
      maxFeePerGas:
        t.max_fee_per_gas != null
          ? String(t.max_fee_per_gas)
          : t.maxFeePerGas != null
            ? String(t.maxFeePerGas)
            : null,
      maxPriorityFeePerGas:
        t.max_priority_fee_per_gas != null
          ? String(t.max_priority_fee_per_gas)
          : t.maxPriorityFeePerGas != null
            ? String(t.maxPriorityFeePerGas)
            : null,
      to: String(t.to ?? ""),
      data: String(t.data ?? ""),
      value: String(t.value ?? "0x0"),
      nonce: Number(t.nonce ?? 0),
      gasEstimate:
        t.gas_estimate != null
          ? Number(t.gas_estimate)
          : t.gasEstimate != null
            ? Number(t.gasEstimate)
            : null,
      gas_estimate:
        t.gas_estimate != null
          ? Number(t.gas_estimate)
          : t.gasEstimate != null
            ? Number(t.gasEstimate)
            : null,
    }),
  );
  const rawResults = Array.isArray(raw.results) ? raw.results : [];
  const results: UpsertItemResult[] = rawResults.map(
    (r: Record<string, unknown>) => ({
      id: r.id != null ? String(r.id) : null,
      key: String(r.key ?? ""),
      op: (r.op ?? "create") as "create" | "patch" | "noop",
    }),
  );

  const f = raw.fees as Record<string, unknown> | undefined;
  const fees: FeeSuggestion | null =
    f != null && typeof f === "object"
      ? {
          maxFeePerGas: String(f.max_fee_per_gas ?? f.maxFeePerGas ?? "0x0"),
          maxPriorityFeePerGas: String(
            f.max_priority_fee_per_gas ?? f.maxPriorityFeePerGas ?? "0x0",
          ),
          source: String(f.source ?? ""),
        }
      : null;
  const from = raw.from != null ? String(raw.from) : null;
  return new PrepareResult({ chainId, transactions, results, from, fees });
}

/**
 * Parse wire JSON into TxResult.
 */
export function parseTxResult(raw: Record<string, unknown>): TxResult {
  const affected = Number(raw.affected ?? 0);
  const rawHashes = Array.isArray(raw.tx_hashes)
    ? raw.tx_hashes
    : Array.isArray(raw.txHashes)
      ? raw.txHashes
      : [];
  const txHashes = rawHashes.map(String);
  const blockNumber =
    typeof raw.block_number === "number"
      ? raw.block_number
      : typeof raw.blockNumber === "number"
        ? raw.blockNumber
        : null;

  return new TxResult({ affected, txHashes, blockNumber });
}
