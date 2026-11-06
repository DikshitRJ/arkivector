/**
 * Arkivector vector types: Vector, HalfVector, Bit, SparseVector
 * Storage:
 *   - Vector: Float32Array
 *   - HalfVector: Uint16Array (raw IEEE binary16 halves)
 *   - Bit: Uint8Array + bitLen
 *   - SparseVector: Int32Array (0-based indices) + Float32Array (values)
 */

import { VectorBytesError } from "./errors.js";
import {
  toHalf,
  fromHalf,
  vectorbytesValidate,
  encodeVector,
  encodeHalfVector,
  encodeBit,
  encodeSparseVector,
} from "./vectorbytes.js";

/**
 * Dense single-precision floating point vector (pgvector `vector`).
 * Storage: Float32Array.
 */
export class Vector {
  readonly data: Float32Array;
  readonly normalized: boolean;

  constructor(
    input: Vector | number[] | Float32Array | Iterable<number> | string,
    normalized: boolean = false
  ) {
    if (input instanceof Vector) {
      this.data = new Float32Array(input.data);
      this.normalized = normalized || input.normalized;
      return;
    }
    this.normalized = normalized;
    if (typeof input === "string") {
      const parsed = Vector.fromText(input);
      this.data = parsed.data;
    } else if (input instanceof Float32Array) {
      this.data = new Float32Array(input);
    } else {
      this.data = Float32Array.from(input);
    }
  }

  get dim(): number {
    return this.data.length;
  }

  toList(): number[] {
    return Array.from(this.data);
  }

  toArray(): Float32Array {
    return new Float32Array(this.data);
  }

  toText(): string {
    return `[${Array.from(this.data).join(",")}]`;
  }

  toString(): string {
    return this.toText();
  }

  toVectorBytes(normalized?: boolean): Uint8Array {
    return encodeVector(this, normalized);
  }

  static fromText(text: string): Vector {
    const trimmed = text.trim();
    if (!trimmed.startsWith("[") || !trimmed.endsWith("]")) {
      throw new Error(`Invalid vector text representation: ${text}`);
    }
    const inner = trimmed.slice(1, -1).trim();
    if (inner.length === 0) {
      return new Vector(new Float32Array(0));
    }
    const parts = inner.split(",");
    const values = new Float32Array(parts.length);
    for (let i = 0; i < parts.length; i++) {
      const val = parseFloat(parts[i].trim());
      if (Number.isNaN(val)) {
        throw new Error(`Invalid float in vector text: ${parts[i]}`);
      }
      values[i] = val;
    }
    return new Vector(values);
  }

  static fromVectorBytes(bytes: Uint8Array): Vector {
    vectorbytesValidate(bytes);
    const type = bytes[2] & 0x0f;
    if (type !== 1) {
      throw new VectorBytesError("VB_UNKNOWN_TYPE", `Expected vector type 1, got ${type}`);
    }
    const normalized = (bytes[3] & 0x01) === 0x01;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const dim = view.getInt16(4, true);
    const data = new Float32Array(dim);
    for (let i = 0; i < dim; i++) {
      data[i] = view.getFloat32(8 + 4 * i, true);
    }
    return new Vector(data, normalized);
  }
}

/**
 * Dense half-precision floating point vector (pgvector `halfvec`).
 * Storage: Uint16Array storing raw binary16 bits.
 */
export class HalfVector {
  readonly raw: Uint16Array;
  readonly normalized: boolean;

  /**
   * Construct HalfVector from numbers (rounded to binary16 round-half-even) or pgvector text format.
   */
  static fromValues(
    input: number[] | Float32Array | Iterable<number> | string,
    normalized: boolean = false
  ): HalfVector {
    return new HalfVector(input, normalized);
  }

  /**
   * Construct HalfVector from already-converted raw uint16 binary16 bits.
   */
  static fromRaw(raw: Uint16Array, normalized: boolean = false): HalfVector {
    const v = Object.create(HalfVector.prototype) as HalfVector;
    Object.defineProperty(v, "raw", { value: new Uint16Array(raw), writable: false });
    Object.defineProperty(v, "normalized", { value: normalized, writable: false });
    return v;
  }

  constructor(
    input: HalfVector | number[] | Float32Array | Iterable<number> | string,
    normalized: boolean = false
  ) {
    if (input instanceof HalfVector) {
      this.raw = new Uint16Array(input.raw);
      this.normalized = normalized || input.normalized;
      return;
    }
    if (typeof input === "string") {
      const parsed = HalfVector.fromText(input);
      this.raw = parsed.raw;
      this.normalized = normalized;
    } else {
      const arr = Array.isArray(input) ? input : Array.from(input);
      const raw = new Uint16Array(arr.length);
      for (let i = 0; i < arr.length; i++) {
        raw[i] = toHalf(arr[i]);
      }
      this.raw = raw;
      this.normalized = normalized;
    }
  }

  get dim(): number {
    return this.raw.length;
  }

  toList(): number[] {
    const out: number[] = new Array(this.raw.length);
    for (let i = 0; i < this.raw.length; i++) {
      out[i] = fromHalf(this.raw[i]);
    }
    return out;
  }

  toFloat32Array(): Float32Array {
    const out = new Float32Array(this.raw.length);
    for (let i = 0; i < this.raw.length; i++) {
      out[i] = fromHalf(this.raw[i]);
    }
    return out;
  }

  toText(): string {
    return `[${this.toList().join(",")}]`;
  }

  toString(): string {
    return this.toText();
  }

  toVectorBytes(normalized?: boolean): Uint8Array {
    return encodeHalfVector(this, normalized);
  }

  static fromText(text: string): HalfVector {
    const trimmed = text.trim();
    if (!trimmed.startsWith("[") || !trimmed.endsWith("]")) {
      throw new Error(`Invalid halfvec text representation: ${text}`);
    }
    const inner = trimmed.slice(1, -1).trim();
    if (inner.length === 0) {
      return new HalfVector(new Uint16Array(0));
    }
    const parts = inner.split(",");
    const raw = new Uint16Array(parts.length);
    for (let i = 0; i < parts.length; i++) {
      const val = parseFloat(parts[i].trim());
      if (Number.isNaN(val)) {
        throw new Error(`Invalid float in halfvec text: ${parts[i]}`);
      }
      raw[i] = toHalf(val);
    }
    return HalfVector.fromRaw(raw);
  }

  static fromVectorBytes(bytes: Uint8Array): HalfVector {
    vectorbytesValidate(bytes);
    const type = bytes[2] & 0x0f;
    if (type !== 2) {
      throw new VectorBytesError("VB_UNKNOWN_TYPE", `Expected halfvec type 2, got ${type}`);
    }
    const normalized = (bytes[3] & 0x01) === 0x01;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const dim = view.getInt16(4, true);
    const raw = new Uint16Array(dim);
    for (let i = 0; i < dim; i++) {
      raw[i] = view.getUint16(8 + 2 * i, true);
    }
    return HalfVector.fromRaw(raw, normalized);
  }
}

/**
 * Binary vector (pgvector `bit`).
 * Storage: Uint8Array with bit length. Bits packed MSB-first.
 */
export class Bit {
  readonly data: Uint8Array;
  readonly bitLen: number;
  readonly normalized: boolean;

  constructor(
    value: Bit | string | boolean[] | number[] | Uint8Array,
    bitLen?: number,
    normalized: boolean = false
  ) {
    if (value instanceof Bit) {
      this.data = new Uint8Array(value.data);
      this.bitLen = value.bitLen;
      this.normalized = normalized || value.normalized;
      return;
    }
    this.normalized = normalized;

    if (typeof value === "string") {
      const clean = value.trim().replace(/^['"]|['"]$/g, "");
      this.bitLen = clean.length;
      const byteLen = Math.ceil(this.bitLen / 8);
      this.data = new Uint8Array(byteLen);
      for (let i = 0; i < this.bitLen; i++) {
        if (clean[i] === "1") {
          this.data[i >>> 3] |= 1 << (7 - (i & 7));
        } else if (clean[i] !== "0") {
          throw new Error(`Invalid bit character: ${clean[i]}`);
        }
      }
    } else if (value instanceof Uint8Array) {
      this.bitLen = bitLen ?? value.length * 8;
      const byteLen = Math.ceil(this.bitLen / 8);
      this.data = new Uint8Array(byteLen);
      this.data.set(value.subarray(0, byteLen));
    } else {
      // boolean[] or number[]
      const arr: (boolean | number)[] = Array.isArray(value)
        ? value
        : Array.from(value as Iterable<boolean | number>);
      this.bitLen = bitLen ?? arr.length;
      const byteLen = Math.ceil(this.bitLen / 8);
      this.data = new Uint8Array(byteLen);
      for (let i = 0; i < this.bitLen; i++) {
        if (arr[i]) {
          this.data[i >>> 3] |= 1 << (7 - (i & 7));
        }
      }
    }
  }

  get dim(): number {
    return this.bitLen;
  }

  toText(): string {
    let out = "";
    for (let i = 0; i < this.bitLen; i++) {
      const bit = (this.data[i >>> 3] >>> (7 - (i & 7))) & 1;
      out += bit ? "1" : "0";
    }
    return out;
  }

  toString(): string {
    return this.toText();
  }

  toBytes(): Uint8Array {
    return new Uint8Array(this.data);
  }

  toList(): number[] {
    const out: number[] = new Array(this.bitLen);
    for (let i = 0; i < this.bitLen; i++) {
      out[i] = (this.data[i >>> 3] >>> (7 - (i & 7))) & 1;
    }
    return out;
  }

  toVectorBytes(normalized?: boolean): Uint8Array {
    return encodeBit(this, normalized);
  }

  static fromText(text: string): Bit {
    return new Bit(text);
  }

  static fromVectorBytes(bytes: Uint8Array): Bit {
    vectorbytesValidate(bytes);
    const type = bytes[2] & 0x0f;
    if (type !== 3) {
      throw new VectorBytesError("VB_UNKNOWN_TYPE", `Expected bit type 3, got ${type}`);
    }
    const normalized = (bytes[3] & 0x01) === 0x01;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const bitLen = view.getInt32(4, true);
    const byteLen = Math.ceil(bitLen / 8);
    const data = new Uint8Array(byteLen);
    data.set(bytes.subarray(8, 8 + byteLen));
    return new Bit(data, bitLen, normalized);
  }
}

/**
 * Compressed sparse vector (pgvector `sparsevec`).
 * Storage: 0-based Int32Array indices + Float32Array values.
 */
export class SparseVector {
  readonly dim: number;
  readonly indices: Int32Array;
  readonly values: Float32Array;
  readonly normalized: boolean;

  constructor(sparse: SparseVector, normalized?: boolean);
  constructor(text: string, normalized?: boolean);
  constructor(dense: number[] | Float32Array, normalized?: boolean);
  constructor(dict: Record<number, number> | Map<number, number>, dim: number, normalized?: boolean);
  constructor(
    dim: number,
    indices: ArrayLike<number>,
    values: ArrayLike<number>,
    normalized?: boolean
  );
  constructor(
    arg1:
      | SparseVector
      | number
      | string
      | number[]
      | Float32Array
      | Record<number, number>
      | Map<number, number>,
    arg2?: number | ArrayLike<number> | boolean,
    arg3?: ArrayLike<number> | boolean,
    arg4?: boolean
  ) {
    if (arg1 instanceof SparseVector) {
      this.dim = arg1.dim;
      this.indices = new Int32Array(arg1.indices);
      this.values = new Float32Array(arg1.values);
      this.normalized = typeof arg2 === "boolean" ? arg2 : arg1.normalized;
      return;
    }
    if (typeof arg1 === "number") {
      // (dim, indices, values, normalized?)
      this.dim = arg1;
      this.indices = Int32Array.from(arg2 as ArrayLike<number>);
      this.values = Float32Array.from(arg3 as ArrayLike<number>);
      this.normalized = typeof arg4 === "boolean" ? arg4 : false;
    } else if (typeof arg1 === "string") {
      // (text, normalized?)
      const parsed = SparseVector.fromText(arg1);
      this.dim = parsed.dim;
      this.indices = parsed.indices;
      this.values = parsed.values;
      this.normalized = typeof arg2 === "boolean" ? arg2 : false;
    } else if (Array.isArray(arg1) || arg1 instanceof Float32Array) {
      // (dense, normalized?)
      this.dim = arg1.length;
      this.normalized = typeof arg2 === "boolean" ? arg2 : false;
      const idxs: number[] = [];
      const vals: number[] = [];
      for (let i = 0; i < arg1.length; i++) {
        const v = arg1[i];
        if (v !== 0) {
          idxs.push(i);
          vals.push(v);
        }
      }
      this.indices = Int32Array.from(idxs);
      this.values = Float32Array.from(vals);
    } else if (typeof arg1 === "object" && typeof arg2 === "number") {
      // (dict/map, dim, normalized?)
      this.dim = arg2;
      this.normalized = typeof arg3 === "boolean" ? arg3 : false;
      const entries: [number, number][] =
        arg1 instanceof Map
          ? Array.from(arg1.entries())
          : Object.entries(arg1).map(([k, v]) => [Number(k), Number(v)]);
      const nonZero = entries.filter(([_, v]) => v !== 0);
      nonZero.sort((a, b) => a[0] - b[0]);
      this.indices = Int32Array.from(nonZero.map((e) => e[0]));
      this.values = Float32Array.from(nonZero.map((e) => e[1]));
    } else {
      throw new TypeError("Invalid arguments for SparseVector constructor");
    }
  }

  get nnz(): number {
    return this.indices.length;
  }

  /**
   * Return dense 0-filled array of size `dim`.
   */
  toList(): number[] {
    const arr = new Array<number>(this.dim).fill(0);
    for (let i = 0; i < this.indices.length; i++) {
      arr[this.indices[i]] = this.values[i];
    }
    return arr;
  }

  toArray(): Float32Array {
    const arr = new Float32Array(this.dim);
    for (let i = 0; i < this.indices.length; i++) {
      arr[this.indices[i]] = this.values[i];
    }
    return arr;
  }

  toDict(): Record<number, number> {
    const dict: Record<number, number> = {};
    for (let i = 0; i < this.indices.length; i++) {
      dict[this.indices[i]] = this.values[i];
    }
    return dict;
  }

  toMap(): Map<number, number> {
    const map = new Map<number, number>();
    for (let i = 0; i < this.indices.length; i++) {
      map.set(this.indices[i], this.values[i]);
    }
    return map;
  }

  /**
   * Format as pgvector 1-based text representation: `{1:1.5,4:-2}/10`.
   */
  toText(): string {
    if (this.indices.length === 0) {
      return `{}/${this.dim}`;
    }
    const pairs: string[] = [];
    for (let i = 0; i < this.indices.length; i++) {
      pairs.push(`${this.indices[i] + 1}:${this.values[i]}`);
    }
    return `{${pairs.join(",") }}/${this.dim}`;
  }

  toString(): string {
    return this.toText();
  }

  toVectorBytes(normalized?: boolean): Uint8Array {
    return encodeSparseVector(this, normalized);
  }

  static fromText(text: string): SparseVector {
    const trimmed = text.trim();
    const slash = trimmed.lastIndexOf("/");
    if (slash === -1) {
      throw new Error(`Invalid sparsevec text representation (missing '/'): ${text}`);
    }
    const dim = parseInt(trimmed.slice(slash + 1).trim(), 10);
    if (Number.isNaN(dim) || dim < 1) {
      throw new Error(`Invalid dimension in sparsevec text: ${trimmed.slice(slash + 1)}`);
    }

    const bracePart = trimmed.slice(0, slash).trim();
    if (!bracePart.startsWith("{") || !bracePart.endsWith("}")) {
      throw new Error(`Invalid sparsevec text representation (missing '{}'): ${text}`);
    }

    const inner = bracePart.slice(1, -1).trim();
    if (inner.length === 0) {
      return new SparseVector(dim, new Int32Array(0), new Float32Array(0));
    }

    const elements = inner.split(",");
    const indices = new Int32Array(elements.length);
    const values = new Float32Array(elements.length);

    for (let i = 0; i < elements.length; i++) {
      const ep = elements[i].split(":", 2);
      if (ep.length !== 2) {
        throw new Error(`Invalid element in sparsevec text: ${elements[i]}`);
      }
      const idx1Based = parseInt(ep[0].trim(), 10);
      const val = parseFloat(ep[1].trim());
      if (Number.isNaN(idx1Based) || idx1Based < 1 || idx1Based > dim) {
        throw new Error(`Invalid index in sparsevec text: ${ep[0]}`);
      }
      if (Number.isNaN(val) || val === 0) {
        throw new Error(`Invalid value in sparsevec text: ${ep[1]}`);
      }
      indices[i] = idx1Based - 1; // Convert 1-based to 0-based
      values[i] = val;
    }

    return new SparseVector(dim, indices, values);
  }

  static fromVectorBytes(bytes: Uint8Array): SparseVector {
    vectorbytesValidate(bytes);
    const type = bytes[2] & 0x0f;
    if (type !== 4) {
      throw new VectorBytesError("VB_UNKNOWN_TYPE", `Expected sparsevec type 4, got ${type}`);
    }
    const normalized = (bytes[3] & 0x01) === 0x01;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const dim = view.getInt32(4, true);
    const nnz = view.getInt32(8, true);
    const indices = new Int32Array(nnz);
    const values = new Float32Array(nnz);
    for (let i = 0; i < nnz; i++) {
      indices[i] = view.getInt32(16 + 4 * i, true);
      values[i] = view.getFloat32(16 + 4 * nnz + 4 * i, true);
    }
    return new SparseVector(dim, indices, values, normalized);
  }
}

/**
 * Union type of all Arkivector vector types.
 */
export type AnyVector = Vector | HalfVector | Bit | SparseVector;
