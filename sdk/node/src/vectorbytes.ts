/**
 * Arkivector VectorBytes v1 encoder and decoder
 * Docs: docs/api/vectors.md §3
 */

import { VectorBytesError } from "./errors.js";
import {
  Vector,
  HalfVector,
  Bit,
  SparseVector,
  type AnyVector,
} from "./types.js";

export const VECTORBYTES_MAGIC_0 = 0x41; // 'A'
export const VECTORBYTES_MAGIC_1 = 0x56; // 'V'
export const VECTORBYTES_VERSION = 1;

export const VectorTypeCode = {
  VECTOR: 1,
  HALFVEC: 2,
  BIT: 3,
  SPARSEVEC: 4,
} as const;

export const VectorBytesCheckOrder = [
  "VB_TOO_SHORT",
  "VB_BAD_MAGIC",
  "VB_BAD_VERSION",
  "VB_UNKNOWN_TYPE",
  "VB_BAD_FLAGS",
  "VB_UNUSED_NONZERO",
  "VB_DIM_OUT_OF_RANGE",
  "VB_NNZ_OUT_OF_RANGE",
  "VB_LENGTH_MISMATCH",
  "VB_NON_FINITE",
  "VB_SPARSE_UNSORTED",
  "VB_SPARSE_INDEX_OUT_OF_RANGE",
  "VB_SPARSE_ZERO_VALUE",
  "VB_BIT_PADDING",
] as const;

const f32Buf = new Float32Array(1);
const u32Buf = new Uint32Array(f32Buf.buffer);

/**
 * Convert a float32-rounded number to IEEE binary16 bits (round-to-nearest-even).
 * Values beyond ±65504 overflow to ±Inf and throw VB_NON_FINITE.
 */
export function toHalf(f: number): number {
  f32Buf[0] = Math.fround(f);
  const x = u32Buf[0];
  const sign = (x >>> 16) & 0x8000;
  const exp = (x >>> 23) & 0xff;
  let mant = x & 0x7fffff;

  if (exp === 0xff) throw new VectorBytesError("VB_NON_FINITE");

  const e = exp - 127 + 15;
  if (e >= 0x1f) throw new VectorBytesError("VB_NON_FINITE"); // overflow

  if (e <= 0) {
    // subnormal / zero
    if (e < -10) return sign;
    mant |= 0x800000;
    const shift = 14 - e;
    let h = mant >>> shift;
    const rem = mant & ((1 << shift) - 1);
    const half = 1 << (shift - 1);
    if (rem > half || (rem === half && (h & 1))) h++;
    return sign | h;
  }

  let h = sign | (e << 10) | (mant >>> 13);
  const rem = mant & 0x1fff;
  if (rem > 0x1000 || (rem === 0x1000 && (h & 1))) h++; // may carry into exponent: correct
  if ((h & 0x7c00) === 0x7c00) throw new VectorBytesError("VB_NON_FINITE");
  return h;
}

/**
 * Convert IEEE binary16 bits to JavaScript number (exact float32 representation).
 */
export function fromHalf(h: number): number {
  const sign = h & 0x8000 ? -1 : 1;
  const exp = (h >>> 10) & 0x1f;
  const mant = h & 0x3ff;

  if (exp === 0) {
    if (mant === 0) return h & 0x8000 ? -0.0 : 0.0;
    return sign * Math.pow(2, -24) * mant;
  } else if (exp === 0x1f) {
    return mant === 0 ? (sign === -1 ? -Infinity : Infinity) : NaN;
  }

  return sign * Math.pow(2, exp - 15) * (1 + mant / 1024);
}

/**
 * Validate VectorBytes v1 bytes in the exact check order specified by docs/api/vectors.md §3.3.
 */
export function vectorbytesValidate(bytes: Uint8Array): void {
  // 1: VB_TOO_SHORT
  if (bytes.length < 8) {
    throw new VectorBytesError("VB_TOO_SHORT");
  }

  // 2: VB_BAD_MAGIC
  if (bytes[0] !== VECTORBYTES_MAGIC_0 || bytes[1] !== VECTORBYTES_MAGIC_1) {
    throw new VectorBytesError("VB_BAD_MAGIC");
  }

  // 3: VB_BAD_VERSION
  if (bytes[2] >>> 4 !== VECTORBYTES_VERSION) {
    throw new VectorBytesError("VB_BAD_VERSION");
  }

  // 4: VB_UNKNOWN_TYPE
  const type = bytes[2] & 0x0f;
  if (type < 1 || type > 4) {
    throw new VectorBytesError("VB_UNKNOWN_TYPE");
  }
  if (type === 4 && bytes.length < 16) {
    throw new VectorBytesError("VB_TOO_SHORT");
  }

  // 5: VB_BAD_FLAGS
  if ((bytes[3] & 0xfe) !== 0) {
    throw new VectorBytesError("VB_BAD_FLAGS");
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  // 6: VB_UNUSED_NONZERO
  if (type === 1 || type === 2) {
    if (view.getUint16(6, true) !== 0) {
      throw new VectorBytesError("VB_UNUSED_NONZERO");
    }
  } else if (type === 4) {
    if (view.getUint32(12, true) !== 0) {
      throw new VectorBytesError("VB_UNUSED_NONZERO");
    }
  }

  // 7: VB_DIM_OUT_OF_RANGE
  if (type === 1 || type === 2) {
    const dim = view.getInt16(4, true);
    if (dim < 1 || dim > 16000) {
      throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
    }
  } else if (type === 3) {
    const dim = view.getInt32(4, true);
    if (dim < 1 || dim > 64000) {
      throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
    }
  } else if (type === 4) {
    const dim = view.getInt32(4, true);
    if (dim < 1 || dim > 1000000000) {
      throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
    }
  }

  // 8: VB_NNZ_OUT_OF_RANGE
  if (type === 4) {
    const dim = view.getInt32(4, true);
    const nnz = view.getInt32(8, true);
    if (nnz < 0 || nnz > dim || nnz > 16000) {
      throw new VectorBytesError("VB_NNZ_OUT_OF_RANGE");
    }
  }

  // 9: VB_LENGTH_MISMATCH
  let expectedLen = 0;
  if (type === 1) {
    const dim = view.getInt16(4, true);
    expectedLen = 8 + 4 * dim;
  } else if (type === 2) {
    const dim = view.getInt16(4, true);
    expectedLen = 8 + 2 * dim;
  } else if (type === 3) {
    const dim = view.getInt32(4, true);
    expectedLen = 8 + Math.ceil(dim / 8);
  } else if (type === 4) {
    const nnz = view.getInt32(8, true);
    expectedLen = 16 + 8 * nnz;
  }

  if (bytes.length !== expectedLen) {
    throw new VectorBytesError("VB_LENGTH_MISMATCH");
  }

  // 10: VB_NON_FINITE
  if (type === 1) {
    const dim = view.getInt16(4, true);
    for (let i = 0; i < dim; i++) {
      const val = view.getFloat32(8 + 4 * i, true);
      if (!Number.isFinite(val)) {
        throw new VectorBytesError("VB_NON_FINITE");
      }
    }
  } else if (type === 2) {
    const dim = view.getInt16(4, true);
    for (let i = 0; i < dim; i++) {
      const h = view.getUint16(8 + 2 * i, true);
      if ((h & 0x7c00) === 0x7c00) {
        throw new VectorBytesError("VB_NON_FINITE");
      }
    }
  } else if (type === 4) {
    const nnz = view.getInt32(8, true);
    for (let i = 0; i < nnz; i++) {
      const val = view.getFloat32(16 + 4 * nnz + 4 * i, true);
      if (!Number.isFinite(val)) {
        throw new VectorBytesError("VB_NON_FINITE");
      }
    }
  }

  // 11: VB_SPARSE_UNSORTED
  if (type === 4) {
    const nnz = view.getInt32(8, true);
    for (let i = 1; i < nnz; i++) {
      const prev = view.getInt32(16 + 4 * (i - 1), true);
      const curr = view.getInt32(16 + 4 * i, true);
      if (curr <= prev) {
        throw new VectorBytesError("VB_SPARSE_UNSORTED");
      }
    }
  }

  // 12: VB_SPARSE_INDEX_OUT_OF_RANGE
  if (type === 4) {
    const dim = view.getInt32(4, true);
    const nnz = view.getInt32(8, true);
    for (let i = 0; i < nnz; i++) {
      const idx = view.getInt32(16 + 4 * i, true);
      if (idx < 0 || idx >= dim) {
        throw new VectorBytesError("VB_SPARSE_INDEX_OUT_OF_RANGE");
      }
    }
  }

  // 13: VB_SPARSE_ZERO_VALUE
  if (type === 4) {
    const nnz = view.getInt32(8, true);
    for (let i = 0; i < nnz; i++) {
      const val = view.getFloat32(16 + 4 * nnz + 4 * i, true);
      if (val === 0) {
        throw new VectorBytesError("VB_SPARSE_ZERO_VALUE");
      }
    }
  }

  // 14: VB_BIT_PADDING
  if (type === 3) {
    const dim = view.getInt32(4, true);
    const rem = dim % 8;
    if (rem !== 0) {
      const mask = (1 << (8 - rem)) - 1;
      const lastByte = bytes[bytes.length - 1];
      if ((lastByte & mask) !== 0) {
        throw new VectorBytesError("VB_BIT_PADDING");
      }
    }
  }
}

/**
 * Encode a dense float32 Vector into VectorBytes v1 format.
 */
export function encodeVector(vector: Vector, normalized?: boolean): Uint8Array {
  const isNorm = normalized ?? vector.normalized;
  const dim = vector.dim;
  if (dim < 1 || dim > 16000) {
    throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
  }

  const out = new Uint8Array(8 + 4 * dim);
  out[0] = VECTORBYTES_MAGIC_0;
  out[1] = VECTORBYTES_MAGIC_1;
  out[2] = 0x11; // version 1, type 1
  out[3] = isNorm ? 0x01 : 0x00;

  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  view.setInt16(4, dim, true);
  view.setInt16(6, 0, true);

  const data = vector.data;
  for (let i = 0; i < dim; i++) {
    const val = data[i];
    if (!Number.isFinite(val)) {
      throw new VectorBytesError("VB_NON_FINITE");
    }
    view.setFloat32(8 + 4 * i, val, true);
  }

  return out;
}

/**
 * Encode a half-precision HalfVector into VectorBytes v1 format.
 */
export function encodeHalfVector(vector: HalfVector, normalized?: boolean): Uint8Array {
  const isNorm = normalized ?? vector.normalized;
  const dim = vector.dim;
  if (dim < 1 || dim > 16000) {
    throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
  }

  const out = new Uint8Array(8 + 2 * dim);
  out[0] = VECTORBYTES_MAGIC_0;
  out[1] = VECTORBYTES_MAGIC_1;
  out[2] = 0x12; // version 1, type 2
  out[3] = isNorm ? 0x01 : 0x00;

  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  view.setInt16(4, dim, true);
  view.setInt16(6, 0, true);

  const raw = vector.raw;
  for (let i = 0; i < dim; i++) {
    const h = raw[i];
    if ((h & 0x7c00) === 0x7c00) {
      throw new VectorBytesError("VB_NON_FINITE");
    }
    view.setUint16(8 + 2 * i, h, true);
  }

  return out;
}

/**
 * Encode a binary Bit vector into VectorBytes v1 format.
 */
export function encodeBit(bit: Bit, normalized?: boolean): Uint8Array {
  const isNorm = normalized ?? bit.normalized;
  const bitLen = bit.bitLen;
  if (bitLen < 1 || bitLen > 64000) {
    throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
  }

  const byteLen = Math.ceil(bitLen / 8);
  const out = new Uint8Array(8 + byteLen);
  out[0] = VECTORBYTES_MAGIC_0;
  out[1] = VECTORBYTES_MAGIC_1;
  out[2] = 0x13; // version 1, type 3
  out[3] = isNorm ? 0x01 : 0x00;

  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  view.setInt32(4, bitLen, true);

  const data = bit.data;
  out.set(data.subarray(0, byteLen), 8);

  const rem = bitLen % 8;
  if (rem !== 0) {
    const mask = (1 << (8 - rem)) - 1;
    if ((out[out.length - 1] & mask) !== 0) {
      throw new VectorBytesError("VB_BIT_PADDING");
    }
  }

  return out;
}

/**
 * Encode a SparseVector into VectorBytes v1 format.
 */
export function encodeSparseVector(sparse: SparseVector, normalized?: boolean): Uint8Array {
  const isNorm = normalized ?? sparse.normalized;
  const dim = sparse.dim;
  const nnz = sparse.nnz;

  if (dim < 1 || dim > 1000000000) {
    throw new VectorBytesError("VB_DIM_OUT_OF_RANGE");
  }
  if (nnz < 0 || nnz > dim || nnz > 16000) {
    throw new VectorBytesError("VB_NNZ_OUT_OF_RANGE");
  }

  const out = new Uint8Array(16 + 8 * nnz);
  out[0] = VECTORBYTES_MAGIC_0;
  out[1] = VECTORBYTES_MAGIC_1;
  out[2] = 0x14; // version 1, type 4
  out[3] = isNorm ? 0x01 : 0x00;

  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  view.setInt32(4, dim, true);
  view.setInt32(8, nnz, true);
  view.setInt32(12, 0, true);

  const indices = sparse.indices;
  const values = sparse.values;

  for (let i = 0; i < nnz; i++) {
    const idx = indices[i];
    const val = values[i];

    if (i > 0 && idx <= indices[i - 1]) {
      throw new VectorBytesError("VB_SPARSE_UNSORTED");
    }
    if (idx < 0 || idx >= dim) {
      throw new VectorBytesError("VB_SPARSE_INDEX_OUT_OF_RANGE");
    }
    if (!Number.isFinite(val)) {
      throw new VectorBytesError("VB_NON_FINITE");
    }
    if (val === 0) {
      throw new VectorBytesError("VB_SPARSE_ZERO_VALUE");
    }

    view.setInt32(16 + 4 * i, idx, true);
    view.setFloat32(16 + 4 * nnz + 4 * i, val, true);
  }

  return out;
}

/**
 * Polymorphic encode function for any Vector type.
 */
export function vectorbytesEncode(vector: AnyVector, normalized?: boolean): Uint8Array {
  if (vector instanceof Vector) {
    return encodeVector(vector, normalized);
  } else if (vector instanceof HalfVector) {
    return encodeHalfVector(vector, normalized);
  } else if (vector instanceof Bit) {
    return encodeBit(vector, normalized);
  } else if (vector instanceof SparseVector) {
    return encodeSparseVector(vector, normalized);
  }
  throw new TypeError("Unsupported vector type for VectorBytes encoding");
}

/**
 * Decode any VectorBytes blob into one of the four vector types (Vector, HalfVector, Bit, SparseVector).
 * Validates the blob strictly before instantiating.
 */
export function vectorbytesDecodeAny(bytes: Uint8Array): AnyVector {
  vectorbytesValidate(bytes);

  const type = bytes[2] & 0x0f;
  const normalized = (bytes[3] & 0x01) === 0x01;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  switch (type) {
    case 1: {
      const dim = view.getInt16(4, true);
      const data = new Float32Array(dim);
      for (let i = 0; i < dim; i++) {
        data[i] = view.getFloat32(8 + 4 * i, true);
      }
      return new Vector(data, normalized);
    }

    case 2: {
      const dim = view.getInt16(4, true);
      const raw = new Uint16Array(dim);
      for (let i = 0; i < dim; i++) {
        raw[i] = view.getUint16(8 + 2 * i, true);
      }
      return HalfVector.fromRaw(raw, normalized);
    }

    case 3: {
      const bitLen = view.getInt32(4, true);
      const byteLen = Math.ceil(bitLen / 8);
      const data = new Uint8Array(byteLen);
      data.set(bytes.subarray(8, 8 + byteLen));
      return new Bit(data, bitLen, normalized);
    }

    case 4: {
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

    default:
      throw new VectorBytesError("VB_UNKNOWN_TYPE");
  }
}
