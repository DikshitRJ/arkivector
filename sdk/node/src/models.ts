/**
 * Arkivector models: TypedValue, T helper, TTL helper, and API interfaces
 * Docs: docs/api/filters.md §2, docs/api/entities.md §7, docs/api/collections.md, docs/api/operations.md
 */

/**
 * Supported attribute types for Arkiv entities.
 */
export type AttrType =
  | "bool"
  | "i32"
  | "u64"
  | "u256"
  | "dec"
  | "str"
  | "addr"
  | "key"
  | "bytes";

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
      throw new TypeError(`Cannot convert value '${String(this.value)}' to number`);
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
      throw new RangeError("seconds must be an even integer between 2 and 15768000");
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
  attrs: Record<string, TypedValue | unknown>
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
    capabilities: Array.isArray(raw.capabilities) ? raw.capabilities.map(String) : [],
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
    lastSyncTime: (raw.last_sync_time ?? raw.lastSyncTime) as string | number | null | undefined,
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
  const defaultTtl = (raw.default_ttl ?? raw.defaultTtl) as TtlInput | null | undefined;
  const open = Boolean(raw.open ?? (membership === "open"));
  const manifestKey = (raw.manifest_key ?? raw.manifestKey) as string | null | undefined;

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
export function serializeTtl(ttl?: TtlInput | null): Record<string, unknown> | null | undefined {
  if (ttl === undefined) return undefined;
  if (ttl === null) return null;
  if ("blocks" in ttl) return { blocks: ttl.blocks };
  if ("seconds" in ttl) return { seconds: ttl.seconds };
  if ("days" in ttl) return { days: ttl.days };
  if ("expires_at_block" in ttl) return { expires_at_block: ttl.expires_at_block };
  if ("expiresAtBlock" in ttl) return { expires_at_block: ttl.expiresAtBlock };
  return ttl as Record<string, unknown>;
}

/**
 * Serialize CreateCollectionOptions into wire JSON CollectionCreate format.
 */
export function serializeCollectionCreate(opts: CreateCollectionOptions): Record<string, unknown> {
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
export function serializeFollowRequest(opts: FollowCollectionOptions): Record<string, unknown> {
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
  throw new Error("FollowCollectionOptions must provide either manifestKey or definition");
}
