# Types

## Vector types

The pgvector types, usable as query vectors, in `EntityInput.vector`, and returned as `hit.vector` / `entity.vector` when you include vectors.

```python
from arkivector import Vector, HalfVector, SparseVector, Bit

Vector([0.1, 0.2, 0.3])                     # float32
HalfVector([0.1, 0.2, 0.3])                 # float16, rounded like pgvector
SparseVector({0: 0.5, 7: 1.0}, dim=10)      # 0-based index -> value
Bit("101100")

v = Vector.from_text("[1,2,3]")
v.to_list(), v.to_text(), v.dim, len(v)
```

Plain lists work everywhere too; the SDK casts them to the collection's column type.

## Typed values

`T` builds typed attribute values for writes and filters:

```python
from arkivector import T

T.str("pets"), T.i32(5), T.u64(10**12), T.u256("1" * 40), T.dec("4.50"), T.bool(True)
T.addr("0x1111111111111111111111111111111111111111"), T.key("0x" + "ab" * 32), T.bytes(b"raw")
```

Responses return attributes as `TypedValue(type, value)`. Large integers and decimals are strings on the wire.

## Ttl

```python
from arkivector import Ttl

Ttl.days(7)            # 0 < days <= 182.5
Ttl.seconds(3600)      # even, 2 .. 15,768,000
Ttl.blocks(1000)       # 1 .. 7,884,000
Ttl.at_block(900000)   # absolute expiry block
```

Plain dicts (`{"days": 7}`) are accepted wherever a `Ttl` is.

## VectorBytes

The on-chain encoding ([VectorBytes](../architecture/vectorbytes.md)). The SDK handles it; to use it directly:

```python
from arkivector import HalfVector, vectorbytes_decode_any

b = HalfVector([1, 0, 0, 0]).to_vectorbytes()
vectorbytes_decode_any(b)        # HalfVector([1.0, 0.0, 0.0, 0.0])
```

Invalid input raises `VectorBytesError` with a `VB_*` code.
