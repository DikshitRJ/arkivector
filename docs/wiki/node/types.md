# Types

## Vector types

```ts
import { Vector, HalfVector, SparseVector, Bit } from "@dikshitrj/arkivector";

new Vector([0.1, 0.2, 0.3]);               // float32
new HalfVector([0.1, 0.2, 0.3]);           // float16, rounded like pgvector
new SparseVector({ 0: 0.5, 7: 1.0 }, 10);  // 0-based index -> value, dimension
new Bit("101100");

const v = Vector.fromText("[1,2,3]");
v.toArray(); v.toText(); v.dim;
```

Plain arrays and `Float32Array` work everywhere; the SDK casts them to the column type.

## Typed values

```ts
import { T } from "@dikshitrj/arkivector";

T.str("pets"); T.i32(5); T.u64("1000000000000"); T.u256("1".repeat(40)); T.dec("4.50"); T.bool(true);
T.addr("0x1111111111111111111111111111111111111111"); T.key("0x" + "ab".repeat(32)); T.bytes(new Uint8Array([1, 2]));
```

Attributes come back as `TypedValue { type, value }`; `plainAttributes(hit.attributes)` turns them into plain values.

## Ttl

```ts
import { Ttl } from "@dikshitrj/arkivector";

Ttl.days(7);           // 0 < days <= 182.5
Ttl.seconds(3600);     // even, 2 .. 15,768,000
Ttl.blocks(1000);      // 1 .. 7,884,000
Ttl.atBlock(900000);   // absolute expiry block
```

Plain objects (`{ days: 7 }`) are accepted too.

## VectorBytes

```ts
import { HalfVector, vectorbytesEncode, vectorbytesDecodeAny } from "@dikshitrj/arkivector";

const bytes = vectorbytesEncode(new HalfVector([1, 0, 0, 0]));
vectorbytesDecodeAny(bytes);   // HalfVector
```

Invalid input throws `VectorBytesError` with a `VB_*` code. Format: [VectorBytes](../architecture/vectorbytes.md).
