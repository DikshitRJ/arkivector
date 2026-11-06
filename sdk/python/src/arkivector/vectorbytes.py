"""VectorBytes v1 binary codec and validation."""

import math
import struct
from typing import TYPE_CHECKING, Any, Dict, Sequence, Tuple, Union

from arkivector.errors import VectorBytesError

if TYPE_CHECKING:
    from arkivector.types import AnyVector

VB_MAGIC = b"AV"
VB_VERSION = 1

T_VECTOR = 1
T_HALFVEC = 2
T_BIT = 3
T_SPARSEVEC = 4

FLAG_NORMALIZED = 0x01

# The 14 check codes in normative validation order
VALIDATION_CHECK_ORDER = [
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
]


def encode_vector(values: Sequence[float], normalized: bool = False) -> bytes:
    """Encode a sequence of float32 values to VectorBytes v1 vector blob."""
    dim = len(values)
    if dim < 1 or dim > 16000:
        raise VectorBytesError("VB_DIM_OUT_OF_RANGE", f"Vector dim {dim} out of range [1, 16000]")

    for v in values:
        if not math.isfinite(v):
            raise VectorBytesError("VB_NON_FINITE", "Vector contains non-finite float value")

    flags = FLAG_NORMALIZED if normalized else 0
    hdr = VB_MAGIC + bytes([(VB_VERSION << 4) | T_VECTOR, flags])
    payload = struct.pack(f"<hh{dim}f", dim, 0, *values)
    return hdr + payload


def encode_halfvec(values: Sequence[float], normalized: bool = False) -> bytes:
    """Encode a sequence of floats to VectorBytes v1 halfvec blob (binary16)."""
    dim = len(values)
    if dim < 1 or dim > 16000:
        raise VectorBytesError(
            "VB_DIM_OUT_OF_RANGE", f"HalfVector dim {dim} out of range [1, 16000]"
        )

    packed_data = bytearray()
    for v in values:
        if not math.isfinite(v):
            raise VectorBytesError("VB_NON_FINITE", "HalfVector contains non-finite float value")
        try:
            p = struct.pack("<e", v)
        except OverflowError:
            raise VectorBytesError("VB_NON_FINITE", f"Value {v} overflows binary16") from None

        unpacked = struct.unpack("<e", p)[0]
        if not math.isfinite(unpacked):
            raise VectorBytesError("VB_NON_FINITE", f"Value {v} converted to non-finite binary16")
        packed_data.extend(p)

    flags = FLAG_NORMALIZED if normalized else 0
    hdr = VB_MAGIC + bytes([(VB_VERSION << 4) | T_HALFVEC, flags]) + struct.pack("<hh", dim, 0)
    return hdr + bytes(packed_data)


def encode_bit(data: bytes, bit_len: int, normalized: bool = False) -> bytes:
    """Encode raw packed bit bytes to VectorBytes v1 bit blob."""
    if bit_len < 1 or bit_len > 64000:
        raise VectorBytesError("VB_DIM_OUT_OF_RANGE", f"Bit dim {bit_len} out of range [1, 64000]")

    expected_len = (bit_len + 7) // 8
    if len(data) != expected_len:
        raise VectorBytesError(
            "VB_LENGTH_MISMATCH",
            f"Bit data length {len(data)} does not match bit_len {bit_len} "
            f"(expected {expected_len})",
        )

    rem = bit_len % 8
    if rem != 0:
        mask = (1 << (8 - rem)) - 1
        if (data[-1] & mask) != 0:
            raise VectorBytesError("VB_BIT_PADDING", "Bit padding bits in last byte are nonzero")

    flags = FLAG_NORMALIZED if normalized else 0
    hdr = VB_MAGIC + bytes([(VB_VERSION << 4) | T_BIT, flags]) + struct.pack("<i", bit_len)
    return hdr + data


def encode_sparse(
    dim: int,
    indices: Sequence[int],
    values: Sequence[float],
    normalized: bool = False,
) -> bytes:
    """Encode sparse (dim, indices, values) to VectorBytes v1 sparsevec blob."""
    if dim < 1 or dim > 1000000000:
        raise VectorBytesError(
            "VB_DIM_OUT_OF_RANGE", f"SparseVector dim {dim} out of range [1, 10^9]"
        )

    nnz = len(indices)
    if len(values) != nnz:
        raise ValueError("indices and values must have identical lengths")

    if nnz < 0 or nnz > dim or nnz > 16000:
        raise VectorBytesError(
            "VB_NNZ_OUT_OF_RANGE",
            f"SparseVector nnz {nnz} out of range [0, min(dim={dim}, 16000)]",
        )

    for v in values:
        if not math.isfinite(v):
            raise VectorBytesError("VB_NON_FINITE", "SparseVector contains non-finite float value")

    for i in range(1, nnz):
        if indices[i] <= indices[i - 1]:
            raise VectorBytesError(
                "VB_SPARSE_UNSORTED",
                f"SparseVector indices must be ascending: {indices[i - 1]} >= {indices[i]}",
            )

    for idx in indices:
        if idx < 0 or idx >= dim:
            raise VectorBytesError(
                "VB_SPARSE_INDEX_OUT_OF_RANGE",
                f"SparseVector index {idx} out of range [0, {dim})",
            )

    for v in values:
        if v == 0.0:
            raise VectorBytesError(
                "VB_SPARSE_ZERO_VALUE", "SparseVector contains explicit 0.0 value"
            )

    flags = FLAG_NORMALIZED if normalized else 0
    hdr = VB_MAGIC + bytes([(VB_VERSION << 4) | T_SPARSEVEC, flags])
    header_body = struct.pack("<iii", dim, nnz, 0)
    idx_body = struct.pack(f"<{nnz}i", *indices)
    val_body = struct.pack(f"<{nnz}f", *values)
    return hdr + header_body + idx_body + val_body


def validate_and_decode(
    data: Union[bytes, bytearray, memoryview],
) -> Tuple[int, int, bool, Dict[str, Any]]:
    """Validate a VectorBytes blob in EXACT check order and decode components.

    Returns:
        (vtype, dim, normalized, payload_dict)
    """
    raw = bytes(data)

    # 1. VB_TOO_SHORT (length >= 8)
    if len(raw) < 8:
        raise VectorBytesError("VB_TOO_SHORT", f"Length {len(raw)} is less than 8")

    # 2. VB_BAD_MAGIC
    if raw[0] != 0x41 or raw[1] != 0x56:
        raise VectorBytesError("VB_BAD_MAGIC", f"Expected magic 'AV', got {raw[:2]!r}")

    # 3. VB_BAD_VERSION
    version = raw[2] >> 4
    if version != 1:
        raise VectorBytesError("VB_BAD_VERSION", f"Expected format version 1, got {version}")

    # 4. VB_UNKNOWN_TYPE
    vtype = raw[2] & 0x0F
    if vtype not in (T_VECTOR, T_HALFVEC, T_BIT, T_SPARSEVEC):
        raise VectorBytesError("VB_UNKNOWN_TYPE", f"Unknown vector type {vtype}")

    # Sparsevec fixed header length is 16 bytes
    if vtype == T_SPARSEVEC and len(raw) < 16:
        raise VectorBytesError("VB_TOO_SHORT", f"SparseVector length {len(raw)} is less than 16")

    # 5. VB_BAD_FLAGS
    flags = raw[3]
    if (flags & 0xFE) != 0:
        raise VectorBytesError("VB_BAD_FLAGS", f"Reserved flag bits set in byte 3: 0x{flags:02x}")
    normalized = bool(flags & FLAG_NORMALIZED)

    # 6. VB_UNUSED_NONZERO
    if vtype in (T_VECTOR, T_HALFVEC):
        unused_16 = struct.unpack_from("<h", raw, 6)[0]
        if unused_16 != 0:
            raise VectorBytesError("VB_UNUSED_NONZERO", f"Unused field nonzero: {unused_16}")
    elif vtype == T_SPARSEVEC:
        unused_32 = struct.unpack_from("<i", raw, 12)[0]
        if unused_32 != 0:
            raise VectorBytesError("VB_UNUSED_NONZERO", f"Unused field nonzero: {unused_32}")

    # 7. VB_DIM_OUT_OF_RANGE
    if vtype in (T_VECTOR, T_HALFVEC):
        dim = struct.unpack_from("<h", raw, 4)[0]
        if dim < 1 or dim > 16000:
            raise VectorBytesError("VB_DIM_OUT_OF_RANGE", f"Dim {dim} out of range [1, 16000]")
    elif vtype == T_BIT:
        dim = struct.unpack_from("<i", raw, 4)[0]
        if dim < 1 or dim > 64000:
            raise VectorBytesError("VB_DIM_OUT_OF_RANGE", f"Bit dim {dim} out of range [1, 64000]")
    elif vtype == T_SPARSEVEC:
        dim = struct.unpack_from("<i", raw, 4)[0]
        if dim < 1 or dim > 1000000000:
            raise VectorBytesError(
                "VB_DIM_OUT_OF_RANGE", f"Sparse dim {dim} out of range [1, 10^9]"
            )
    else:
        raise VectorBytesError("VB_UNKNOWN_TYPE")

    # 8. VB_NNZ_OUT_OF_RANGE (sparsevec only)
    nnz = 0
    if vtype == T_SPARSEVEC:
        nnz = struct.unpack_from("<i", raw, 8)[0]
        if nnz < 0 or nnz > dim or nnz > 16000:
            raise VectorBytesError(
                "VB_NNZ_OUT_OF_RANGE",
                f"Sparse nnz {nnz} out of range [0, min({dim}, 16000)]",
            )

    # 9. VB_LENGTH_MISMATCH
    if vtype == T_VECTOR:
        expected_len = 8 + 4 * dim
    elif vtype == T_HALFVEC:
        expected_len = 8 + 2 * dim
    elif vtype == T_BIT:
        expected_len = 8 + (dim + 7) // 8
    elif vtype == T_SPARSEVEC:
        expected_len = 16 + 8 * nnz
    else:
        expected_len = -1

    if len(raw) != expected_len:
        raise VectorBytesError(
            "VB_LENGTH_MISMATCH",
            f"Length {len(raw)} does not match expected {expected_len}",
        )

    # 10. VB_NON_FINITE
    payload: Dict[str, Any] = {}
    if vtype == T_VECTOR:
        values = list(struct.unpack_from(f"<{dim}f", raw, 8))
        for v in values:
            if not math.isfinite(v):
                raise VectorBytesError("VB_NON_FINITE", "Vector contains non-finite float")
        payload["values"] = values
    elif vtype == T_HALFVEC:
        half_values = list(struct.unpack_from(f"<{dim}e", raw, 8))
        for v in half_values:
            if not math.isfinite(v):
                raise VectorBytesError("VB_NON_FINITE", "HalfVector contains non-finite float")
        payload["values"] = half_values
        payload["raw_bytes"] = raw[8:]
    elif vtype == T_SPARSEVEC:
        if nnz > 0:
            sparse_values = list(struct.unpack_from(f"<{nnz}f", raw, 16 + 4 * nnz))
            for v in sparse_values:
                if not math.isfinite(v):
                    raise VectorBytesError(
                        "VB_NON_FINITE", "SparseVector contains non-finite float"
                    )
            payload["values"] = sparse_values
        else:
            payload["values"] = []
    elif vtype == T_BIT:
        payload["data_bytes"] = raw[8:]

    # 11. VB_SPARSE_UNSORTED
    if vtype == T_SPARSEVEC and nnz > 0:
        indices = list(struct.unpack_from(f"<{nnz}i", raw, 16))
        for i in range(1, nnz):
            if indices[i] <= indices[i - 1]:
                raise VectorBytesError(
                    "VB_SPARSE_UNSORTED",
                    f"Sparse indices unsorted: {indices[i - 1]} >= {indices[i]}",
                )
        payload["indices"] = indices
    elif vtype == T_SPARSEVEC:
        payload["indices"] = []

    # 12. VB_SPARSE_INDEX_OUT_OF_RANGE
    if vtype == T_SPARSEVEC and nnz > 0:
        indices = payload["indices"]
        for idx in indices:
            if idx < 0 or idx >= dim:
                raise VectorBytesError(
                    "VB_SPARSE_INDEX_OUT_OF_RANGE",
                    f"Sparse index {idx} out of range [0, {dim})",
                )

    # 13. VB_SPARSE_ZERO_VALUE
    if vtype == T_SPARSEVEC and nnz > 0:
        sparse_values = payload["values"]
        for v in sparse_values:
            if v == 0.0:
                raise VectorBytesError(
                    "VB_SPARSE_ZERO_VALUE",
                    "Sparse value cannot be explicit zero",
                )

    # 14. VB_BIT_PADDING
    if vtype == T_BIT:
        rem = dim % 8
        if rem != 0:
            last_byte = raw[-1]
            unused_mask = (1 << (8 - rem)) - 1
            if (last_byte & unused_mask) != 0:
                raise VectorBytesError(
                    "VB_BIT_PADDING",
                    f"Bit padding bits in last byte are nonzero (0x{last_byte:02x})",
                )

    return vtype, dim, normalized, payload


def vectorbytes_decode_any(data: Union[bytes, bytearray, memoryview]) -> "AnyVector":
    """Decode any VectorBytes v1 blob into the matching SDK vector type."""
    from arkivector.types import Bit, HalfVector, SparseVector, Vector

    vtype, dim, normalized, payload = validate_and_decode(data)

    if vtype == T_VECTOR:
        return Vector(payload["values"], normalized=normalized)
    if vtype == T_HALFVEC:
        # Construct directly from unpacked binary16 floats or raw bytes
        vec = HalfVector.__new__(HalfVector)
        vec._packed = payload["raw_bytes"]
        vec._floats = payload["values"]
        vec.normalized = normalized
        return vec
    if vtype == T_BIT:
        return Bit(payload["data_bytes"], bit_len=dim, normalized=normalized)
    if vtype == T_SPARSEVEC:
        return SparseVector(dim, payload["indices"], payload["values"], normalized=normalized)

    raise VectorBytesError("VB_UNKNOWN_TYPE", f"Unknown vector type {vtype}")
