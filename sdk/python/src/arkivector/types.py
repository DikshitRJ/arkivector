"""Arkivector vector types: Vector, HalfVector, SparseVector, Bit."""

import math
import struct
from array import array
from collections.abc import Iterable
from typing import (
    Any,
    Dict,
    List,
    Optional,
    Sequence,
    Union,
)

from arkivector.errors import VectorBytesError
from arkivector.vectorbytes import (
    FLAG_NORMALIZED,
    T_HALFVEC,
    VB_MAGIC,
    VB_VERSION,
    encode_bit,
    encode_sparse,
    encode_vector,
    vectorbytes_decode_any,
)


def _extract_float_list(value: Any) -> List[float]:
    """Extract float list from duck-typed sequences, numpy arrays, or pgvector types."""
    if hasattr(value, "to_list") and callable(value.to_list):
        return [float(x) for x in value.to_list()]
    if hasattr(value, "tolist") and callable(value.tolist):
        return [float(x) for x in value.tolist()]
    if isinstance(value, (list, tuple)):
        return [float(x) for x in value]
    if isinstance(value, Iterable) and not isinstance(
        value, (str, bytes, bytearray, memoryview, dict)
    ):
        return [float(x) for x in value]
    raise TypeError(f"Cannot convert {type(value).__name__} to float list")


class Vector:
    """32-bit single-precision floating point dense vector (pgvector vector)."""

    def __init__(
        self,
        values: Union[Sequence[float], Any],
        *,
        normalized: bool = False,
    ) -> None:
        raw_list = _extract_float_list(values)
        for v in raw_list:
            if not math.isfinite(v):
                raise VectorBytesError("VB_NON_FINITE", f"Non-finite float value in Vector: {v}")
        self._values: array[float] = array("f", raw_list)
        self.normalized: bool = normalized

    @property
    def dim(self) -> int:
        """Vector dimensionality."""
        return len(self._values)

    def dimensions(self) -> int:
        """Alias for dim matching pgvector-python."""
        return len(self._values)

    def to_list(self) -> List[float]:
        """Convert elements to a Python float list."""
        return list(self._values)

    def to_text(self) -> str:
        """Format as pgvector text literal: '[1,2,3]'."""
        return f"[{','.join(str(v) for v in self._values)}]"

    @classmethod
    def from_text(cls, s: str) -> "Vector":
        """Parse from a pgvector text literal: '[1,2,3]'."""
        clean = s.strip()
        if not (clean.startswith("[") and clean.endswith("]")):
            raise ValueError(f"Invalid Vector text format: {s}")
        inner = clean[1:-1].strip()
        if not inner:
            return cls([])
        parts = [float(p.strip()) for p in inner.split(",") if p.strip()]
        return cls(parts)

    def to_vectorbytes(self, normalized: Optional[bool] = None) -> bytes:
        """Encode to VectorBytes v1 bytes."""
        norm = self.normalized if normalized is None else normalized
        return encode_vector(self._values, normalized=norm)

    @classmethod
    def from_vectorbytes(cls, b: bytes) -> "Vector":
        """Decode VectorBytes v1 bytes into a Vector."""
        vec = vectorbytes_decode_any(b)
        if not isinstance(vec, Vector):
            raise ValueError(f"Expected Vector blob, got {type(vec).__name__}")
        return vec

    def __len__(self) -> int:
        return len(self._values)

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Vector):
            return self._values == other._values
        if hasattr(other, "to_list") and callable(other.to_list):
            return self.to_list() == [float(x) for x in other.to_list()]
        return NotImplemented

    def __repr__(self) -> str:
        return f"Vector({self.to_list()})"


class HalfVector:
    """16-bit half-precision floating point dense vector (pgvector halfvec)."""

    def __init__(
        self,
        values: Union[Sequence[float], Any],
        *,
        normalized: bool = False,
    ) -> None:
        raw_list = _extract_float_list(values)
        packed_bytes = bytearray()
        unpacked_floats: List[float] = []

        for x in raw_list:
            if not math.isfinite(x):
                raise VectorBytesError(
                    "VB_NON_FINITE", f"Non-finite float value in HalfVector: {x}"
                )
            try:
                p = struct.pack("<e", x)
            except OverflowError:
                raise VectorBytesError(
                    "VB_NON_FINITE",
                    f"Value {x} overflows binary16 range [-65504, 65504]",
                ) from None

            u = struct.unpack("<e", p)[0]
            if not math.isfinite(u):
                raise VectorBytesError(
                    "VB_NON_FINITE",
                    f"Value {x} overflows binary16 range [-65504, 65504]",
                )
            packed_bytes.extend(p)
            unpacked_floats.append(u)

        self._packed: bytes = bytes(packed_bytes)
        self._floats: List[float] = unpacked_floats
        self.normalized: bool = normalized

    @property
    def dim(self) -> int:
        """Vector dimensionality."""
        return len(self._floats)

    def dimensions(self) -> int:
        """Alias for dim matching pgvector-python."""
        return len(self._floats)

    def to_list(self) -> List[float]:
        """Convert elements to a Python float list (exact binary16 decoded values)."""
        return list(self._floats)

    def to_text(self) -> str:
        """Format as pgvector text literal: '[1,2,3]'."""
        return f"[{','.join(str(v) for v in self._floats)}]"

    @classmethod
    def from_text(cls, s: str) -> "HalfVector":
        """Parse from a pgvector text literal: '[1,2,3]'."""
        clean = s.strip()
        if not (clean.startswith("[") and clean.endswith("]")):
            raise ValueError(f"Invalid HalfVector text format: {s}")
        inner = clean[1:-1].strip()
        if not inner:
            return cls([])
        parts = [float(p.strip()) for p in inner.split(",") if p.strip()]
        return cls(parts)

    def to_vectorbytes(self, normalized: Optional[bool] = None) -> bytes:
        """Encode to VectorBytes v1 bytes."""
        norm = self.normalized if normalized is None else normalized
        if self.dim < 1 or self.dim > 16000:
            raise VectorBytesError(
                "VB_DIM_OUT_OF_RANGE", f"HalfVector dim {self.dim} out of range [1, 16000]"
            )
        flags = FLAG_NORMALIZED if norm else 0
        hdr = (
            VB_MAGIC
            + bytes([(VB_VERSION << 4) | T_HALFVEC, flags])
            + struct.pack("<hh", self.dim, 0)
        )
        return hdr + self._packed

    @classmethod
    def from_vectorbytes(cls, b: bytes) -> "HalfVector":
        """Decode VectorBytes v1 bytes into a HalfVector."""
        vec = vectorbytes_decode_any(b)
        if not isinstance(vec, HalfVector):
            raise ValueError(f"Expected HalfVector blob, got {type(vec).__name__}")
        return vec

    def __len__(self) -> int:
        return len(self._floats)

    def __eq__(self, other: object) -> bool:
        if isinstance(other, HalfVector):
            return self._packed == other._packed
        if hasattr(other, "to_list") and callable(other.to_list):
            return self.to_list() == [float(x) for x in other.to_list()]
        return NotImplemented

    def __repr__(self) -> str:
        return f"HalfVector({self.to_list()})"


class Bit:
    """1-bit binary vector / bit string (pgvector bit/varbit)."""

    def __init__(
        self,
        bits: Union[str, Sequence[bool], bytes, bytearray, memoryview, Any],
        bit_len: Optional[int] = None,
        *,
        normalized: bool = False,
    ) -> None:
        self.normalized: bool = normalized

        if isinstance(bits, str):
            clean = bits.strip()
            length = len(clean)
            data = bytearray((length + 7) // 8)
            for i, ch in enumerate(clean):
                if ch == "1":
                    data[i // 8] |= 0x80 >> (i % 8)
                elif ch != "0":
                    raise ValueError(f"Invalid character in bit string: {ch!r}")
            self._length: int = length
            self._data: bytes = bytes(data)

        elif isinstance(bits, (bytes, bytearray, memoryview)):
            raw_data = bytes(bits)
            length = len(raw_data) * 8 if bit_len is None else bit_len
            expected_bytes = (length + 7) // 8
            if len(raw_data) != expected_bytes:
                raise ValueError(
                    f"Data length {len(raw_data)} does not match bit_len {length} "
                    f"(expected {expected_bytes})"
                )
            rem = length % 8
            if rem != 0:
                mask = (1 << (8 - rem)) - 1
                if (raw_data[-1] & mask) != 0:
                    raise VectorBytesError(
                        "VB_BIT_PADDING", "Bit padding bits in last byte are nonzero"
                    )
            self._length = length
            self._data = raw_data

        elif hasattr(bits, "to_text") and callable(bits.to_text):
            text = bits.to_text().strip()
            length = len(text)
            data = bytearray((length + 7) // 8)
            for i, ch in enumerate(text):
                if ch == "1":
                    data[i // 8] |= 0x80 >> (i % 8)
            self._length = length
            self._data = bytes(data)

        elif hasattr(bits, "tolist") and callable(bits.tolist):
            bools = [bool(x) for x in bits.tolist()]
            length = len(bools)
            data = bytearray((length + 7) // 8)
            for i, b in enumerate(bools):
                if b:
                    data[i // 8] |= 0x80 >> (i % 8)
            self._length = length
            self._data = bytes(data)

        elif isinstance(bits, (list, tuple)):
            bools = [bool(x) for x in bits]
            length = len(bools)
            data = bytearray((length + 7) // 8)
            for i, b in enumerate(bools):
                if b:
                    data[i // 8] |= 0x80 >> (i % 8)
            self._length = length
            self._data = bytes(data)

        else:
            raise TypeError(f"Cannot initialize Bit from {type(bits).__name__}")

    @property
    def dim(self) -> int:
        """Number of bits."""
        return self._length

    def dimensions(self) -> int:
        """Alias for dim."""
        return self._length

    def to_text(self) -> str:
        """Format as bit string literal: '1010...'."""
        return "".join(format(b, "08b") for b in self._data)[: self._length]

    def to_bytes(self) -> bytes:
        """Return the packed bytes."""
        return self._data

    def to_list(self) -> List[bool]:
        """Convert bits to a list of booleans."""
        return [ch == "1" for ch in self.to_text()]

    @classmethod
    def from_text(cls, s: str) -> "Bit":
        """Parse from a bit string literal: '1010...'."""
        return cls(s.strip())

    def to_vectorbytes(self, normalized: Optional[bool] = None) -> bytes:
        """Encode to VectorBytes v1 bytes."""
        norm = self.normalized if normalized is None else normalized
        return encode_bit(self._data, self._length, normalized=norm)

    @classmethod
    def from_vectorbytes(cls, b: bytes) -> "Bit":
        """Decode VectorBytes v1 bytes into a Bit."""
        vec = vectorbytes_decode_any(b)
        if not isinstance(vec, Bit):
            raise ValueError(f"Expected Bit blob, got {type(vec).__name__}")
        return vec

    def __len__(self) -> int:
        return self._length

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Bit):
            return self._length == other._length and self._data == other._data
        if hasattr(other, "to_text") and callable(other.to_text):
            return self.to_text() == str(other.to_text())
        return NotImplemented

    def __repr__(self) -> str:
        return f"Bit('{self.to_text()}')"


class SparseVector:
    """Compressed sparse vector (pgvector sparsevec).

    Indices are 0-based in memory and wire format, and 1-based in pgvector text format.
    """

    def __init__(
        self,
        arg1: Union[int, Dict[int, float], str, Any],
        arg2: Optional[Union[Sequence[int], int, Dict[int, float]]] = None,
        arg3: Optional[Sequence[float]] = None,
        *,
        dim: Optional[int] = None,
        indices: Optional[Sequence[int]] = None,
        values: Optional[Sequence[float]] = None,
        normalized: bool = False,
    ) -> None:
        self.normalized: bool = normalized

        # Case 1: SparseVector("{1:1.5,4:-2}/10")
        if isinstance(arg1, str):
            parsed = self.from_text(arg1)
            self._dim: int = parsed.dim
            self._indices: List[int] = parsed.indices()
            self._values: List[float] = parsed.values()
            return

        # Case 2: SparseVector(dict, dim) or SparseVector(dict, dim=10)
        if isinstance(arg1, dict):
            resolved_dim = int(
                dim if dim is not None else (int(arg2) if isinstance(arg2, int) else 0)
            )
            sorted_items = sorted((int(k), float(v)) for k, v in arg1.items() if float(v) != 0.0)
            if resolved_dim <= 0 and sorted_items:
                resolved_dim = max(k for k, _ in sorted_items) + 1
            self._dim = resolved_dim
            self._indices = [k for k, _ in sorted_items]
            self._values = [v for _, v in sorted_items]
            return

        # Case 3: SparseVector(dim, dict)
        if isinstance(arg1, int) and isinstance(arg2, dict):
            sorted_items = sorted((int(k), float(v)) for k, v in arg2.items() if float(v) != 0.0)
            self._dim = arg1
            self._indices = [k for k, _ in sorted_items]
            self._values = [v for _, v in sorted_items]
            return

        # Case 4: SparseVector(dim, indices, values)
        if isinstance(arg1, int):
            self._dim = arg1
            idx_seq = (
                indices
                if indices is not None
                else (arg2 if isinstance(arg2, (list, tuple, Sequence)) else None)
            )
            val_seq = values if values is not None else arg3
            self._indices = [int(i) for i in idx_seq] if idx_seq is not None else []
            self._values = [float(v) for v in val_seq] if val_seq is not None else []
            return

        # Case 5: Duck-typing another SparseVector / pgvector / SciPy
        if hasattr(arg1, "indices") and hasattr(arg1, "values") and hasattr(arg1, "dimensions"):
            self._dim = int(arg1.dimensions())
            self._indices = [int(i) for i in arg1.indices()]
            self._values = [float(v) for v in arg1.values()]
            return

        if hasattr(arg1, "tocoo"):
            coo = arg1.tocoo()
            self._dim = int(coo.shape[1] if len(coo.shape) > 1 else coo.shape[0])
            sorted_coo = sorted(zip(coo.col, coo.data), key=lambda x: x[0])
            self._indices = [int(i) for i, _ in sorted_coo]
            self._values = [float(v) for _, v in sorted_coo]
            return

        raise TypeError(
            f"Cannot initialize SparseVector from arguments: {arg1!r}, {arg2!r}, {arg3!r}"
        )

    @property
    def dim(self) -> int:
        """Vector dimensionality."""
        return self._dim

    def dimensions(self) -> int:
        """Alias for dim."""
        return self._dim

    @property
    def nnz(self) -> int:
        """Number of non-zero entries."""
        return len(self._indices)

    def indices(self) -> List[int]:
        """0-based non-zero index list."""
        return list(self._indices)

    def values(self) -> List[float]:
        """Non-zero float value list."""
        return list(self._values)

    def to_dict(self) -> Dict[int, float]:
        """Convert to a 0-based {index: value} dict."""
        return dict(zip(self._indices, self._values))

    def to_list(self) -> List[float]:
        """Convert to a dense Python float list of length dim."""
        res = [0.0] * self._dim
        for idx, val in zip(self._indices, self._values):
            if 0 <= idx < self._dim:
                res[idx] = val
        return res

    def to_text(self) -> str:
        """Format as pgvector text literal (1-based indices): '{1:1.5,4:-2}/10'."""
        elements = ",".join(f"{i + 1}:{v:g}" for i, v in zip(self._indices, self._values))
        return f"{{{elements}}}/{self._dim}"

    @classmethod
    def from_text(cls, s: str) -> "SparseVector":
        """Parse from a pgvector text literal (1-based indices): '{1:1.5,4:-2}/10'."""
        clean = s.strip()
        if "/" not in clean:
            raise ValueError(f"Invalid SparseVector text format: {s}")
        elements_str, dim_str = clean.split("/", 1)
        dim = int(dim_str.strip())
        elements_str = elements_str.strip()
        if not (elements_str.startswith("{") and elements_str.endswith("}")):
            raise ValueError(f"Invalid SparseVector text format: {s}")
        inner = elements_str[1:-1].strip()

        indices: List[int] = []
        values: List[float] = []
        if inner:
            for item in inner.split(","):
                item = item.strip()
                if not item:
                    continue
                idx_str, val_str = item.split(":", 1)
                idx = int(idx_str.strip()) - 1
                val = float(val_str.strip())
                indices.append(idx)
                values.append(val)

        return cls(dim, indices, values)

    def to_vectorbytes(self, normalized: Optional[bool] = None) -> bytes:
        """Encode to VectorBytes v1 bytes."""
        norm = self.normalized if normalized is None else normalized
        return encode_sparse(self._dim, self._indices, self._values, normalized=norm)

    @classmethod
    def from_vectorbytes(cls, b: bytes) -> "SparseVector":
        """Decode VectorBytes v1 bytes into a SparseVector."""
        vec = vectorbytes_decode_any(b)
        if not isinstance(vec, SparseVector):
            raise ValueError(f"Expected SparseVector blob, got {type(vec).__name__}")
        return vec

    def __eq__(self, other: object) -> bool:
        if isinstance(other, SparseVector):
            return (
                self._dim == other._dim
                and self._indices == other._indices
                and self._values == other._values
            )
        return NotImplemented

    def __repr__(self) -> str:
        return f"SparseVector({self._dim}, {self._indices}, {self._values})"


AnyVector = Union[Vector, HalfVector, SparseVector, Bit]
