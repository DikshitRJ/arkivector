/**
 * Arkivector models: TypedValue, T helper, TTL helper
 * Docs: docs/api/filters.md §2, docs/api/entities.md §7
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
