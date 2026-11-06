# Attribute Serialization & ABI Memory Specification

## Table of Contents
- [1. Executive Summary](#1-executive-summary)
- [2. Solidity Attribute Struct Definition](#2-solidity-attribute-struct-definition)
- [3. Name Packing & Identifier Specification](#3-name-packing--identifier-specification)
  - [3.1 Left-Aligned ASCII Word Packing](#31-left-aligned-ascii-word-packing)
  - [3.2 Lexical Naming Grammar & Regex Rules](#32-lexical-naming-grammar--regex-rules)
  - [3.3 Reserved Names & System Attribute Protection](#33-reserved-names--system-attribute-protection)
  - [3.4 The `arkiv.schema` Tag](#34-the-arkivschema-tag)
- [4. Type Identifier (`typeId`) Bitfield Layout](#4-type-identifier-typeid-bitfield-layout)
  - [4.1 The 7-Bit Type Code + Bit 7 Indexed Flag](#41-the-7-bit-type-code--bit-7-indexed-flag)
  - [4.2 Comprehensive Type Catalog Matrix](#42-comprehensive-type-catalog-matrix)
- [5. Value Byte Serialization & Word Alignment](#5-value-byte-serialization--word-alignment)
  - [5.1 Single-Word 32-Byte Primitive Types](#51-single-word-32-byte-primitive-types)
  - [5.2 Variable-Length Types (`str`, `bytes`)](#52-variable-length-types-str-bytes)
  - [5.3 Tombstone Specification (Type 0)](#53-tombstone-specification-type-0)
- [6. Canonical Attribute Ordering & Cardinality Invariants](#6-canonical-attribute-ordering--cardinality-invariants)
  - [6.1 Strict Lexicographical Ascending Sort](#61-strict-lexicographical-ascending-sort)
  - [6.2 Uniqueness Guarantee](#62-uniqueness-guarantee)
  - [6.3 Attribute Count Limits](#63-attribute-count-limits)
- [7. Complete Calldata Byte Breakdown](#7-complete-calldata-byte-breakdown)
- [8. Reference Serialization Implementations](#8-reference-serialization-implementations)

---

## 1. Executive Summary

In Arkiv, entities do not conform to hardcoded on-chain database schemas. Instead, an entity is a collection of polymorphic key-value attributes stored within the EVM state trie. The underlying smart contract enforces strong typing, memory bounds, and index registration through a standardized serialization format: the `Attribute` struct.

Understanding how attribute names are padded, how the 8-bit `typeId` encodes both data type and indexability, how values are aligned into 32-byte words, and why attributes must be sorted in strictly ascending byte order is essential for building compliant Arkiv SDKs and smart contract integrations.

---

## 2. Solidity Attribute Struct Definition

The core data structure representing an individual entity attribute is defined as follows:

```solidity
struct Attribute {
    bytes32 name;    // ASCII identifier, left-aligned, zero-padded to 32 bytes
    uint8 typeId;    // 7-bit type code | (isIndexed ? 0x80 : 0x00)
    bytes value;     // Serialized payload (fixed 32 bytes, variable string/bytes, or empty)
}
```

When encoded as an array of attributes (`Attribute[]`), each attribute is represented within Solidity ABI calldata as a dynamic tuple due to the dynamic `bytes value` field.

---

## 3. Name Packing & Identifier Specification

### 3.1 Left-Aligned ASCII Word Packing

Attribute names in Arkiv are stored as `bytes32`. The string is converted to ASCII/UTF-8 bytes, aligned to the **left** (most significant byte), and padded with null bytes (`0x00`) to fill the remainder of the 32-byte word:

```text
Attribute Name: "category"
ASCII Bytes:    0x63 0x61 0x74 0x65 0x67 0x6f 0x72 0x79 (8 bytes)

Padded bytes32:
0x63617465676f7279000000000000000000000000000000000000000000000000
|<--- 8 bytes --->|<------------------- 24 zero bytes ------------------->|
```

> [!CAUTION]
> Right-aligning or right-padding attribute names with spaces or non-zero bytes violates canonical encoding and results in immediate contract reverts or indexing mismatches.

### 3.2 Lexical Naming Grammar & Regex Rules

Arkiv enforces strict validation on attribute names:

$$\text{AttributeName} \in \mathcal{L}\left( \texttt{^[a-zA-Z][a-zA-Z0-9.\_-]{0,31}\$} \right)$$

1. **Initial Character**: Must begin with an ASCII alphabetical letter (`a-z` or `A-Z`). Names cannot start with digits, periods, underscores, or hyphens.
2. **Subsequent Characters**: May contain ASCII alphanumeric characters (`a-z`, `A-Z`, `0-9`), periods (`.`), hyphens (`-`), or underscores (`_`).
3. **Maximum Length**: At most **32 bytes** (`MAX_NAME_BYTES`).
4. **Case Sensitivity**: Identifiers are strictly case-sensitive. `category` and `Category` are two distinct attributes with different `bytes32` values.
5. **No Double Hyphen**: Names must **not** contain consecutive hyphens (`--`), as this sequence is reserved for SQL comments in the query engine.
6. **Query Language Reserved Words**: Names cannot collide with query language keywords:
   - Reserved keywords: `and`, `or`, `not`, `between`, `order`, `by`, `asc`, `desc`, `select`, `from`, `where`, `limit`, `offset`, `exists`, `like`, `is`, `null`.
   - Reserved type names: `bool`, `i32`, `u64`, `u256`, `dec`, `str`, `bytes`, `addr`, `key`.

#### Error Revert Codes:
- `Ident32Empty()` (selector `0xc109aef4`): Name length is 0.
- `Ident32InvalidByte(uint256 position, bytes1 value)` (selector `0x276f7798`): Character at `position` violates lexical grammar.

### 3.3 Reserved Names & System Attribute Protection

System fields always begin with the dollar sign character (`$`, ASCII `0x24`):

```text
$key, $owner, $creator, $createdAt, $expiresAt, $creationFlags, $attributes, $attributeSchema
```

User-defined attribute names must **never** begin with `$`. Attempting to create or patch an attribute starting with `$` triggers the EVM custom revert:

```solidity
revert SystemAttributeNotWritable(bytes32 name); // Selector: 0x79b2b398
```

This ensures zero ambiguity between protocol-managed system metadata and user domain data.

### 3.4 The `arkiv.schema` Tag

A named schema automatically attaches a metadata attribute named `arkiv.schema`.
- **Name**: `arkiv.schema` (padded to `0x61726b69762e736368656d610000000000000000000000000000000000000000`).
- **Type**: `str` (`typeId = 8`), **indexed** (`typeId = 0x88`).
- **Value**: UTF-8 encoded schema project tag (e.g., `my-app.posts`).
- **Isolation Purpose**: Allows indexing nodes to partition and filter entities per application project without dedicated contract deployments.
- **Reserved Status**: A user schema cannot define a column explicitly named `arkiv.schema`. Attempting to do so in SDK definitions throws `SchemaDefinitionError`.

---

## 4. Type Identifier (`typeId`) Bitfield Layout

### 4.1 The 7-Bit Type Code + Bit 7 Indexed Flag

The `typeId` is a single unsigned 8-bit integer (`uint8`, range `0..255`). It multiplexes the base data type and the indexing flag:

```text
Bit 7          Bit 6  Bit 5  Bit 4  Bit 3  Bit 2  Bit 1  Bit 0
┌────────────┬─────────────────────────────────────────────────┐
│  INDEXED   │                  TYPE CODE                      │
│ (1 = Yes)  │                 (Values 0..10)                  │
└────────────┴─────────────────────────────────────────────────┘
  0x80 bit                      0x00 .. 0x7F
```

- **Type Code Extraction**: `typeCode = typeId & 0x7F`
- **Indexed Flag Extraction**: `isIndexed = (typeId & 0x80) != 0`
- **Indexed Type Construction**: `typeId = typeCode | 0x80`

### 4.2 Comprehensive Type Catalog Matrix

| Type Code | Type Name | Unindexed `typeId` (Hex) | Indexed `typeId` (Hex) | Solidity `bytes value` Payload Format | Max Storage / Range | Indexing Allowed? |
| :---: | :---: | :---: | :---: | :--- | :--- | :---: |
| **`0`** | **Tombstone** | `0` (`0x00`) | N/A | Empty (`0x`, length 0). Unsets column in patch. | 0 bytes | ❌ No |
| **`1`** | **`bool`** | `1` (`0x01`) | `129` (`0x81`) | 32 bytes: `0` (`false`) or `1` (`true`). | 1 bit in 32-byte word | ✅ Yes |
| **`2`** | **`i32`** | `2` (`0x02`) | `130` (`0x82`) | 32 bytes: two's complement sign-extended integer. | $-2^{31} \dots 2^{31}-1$ | ✅ Yes |
| **`3`** | **`u64`** | `3` (`0x03`) | `131` (`0x83`) | 32 bytes: right-aligned unsigned integer. | $0 \dots 2^{64}-1$ | ✅ Yes |
| **`4`** | **`u256`** | `4` (`0x04`) | `132` (`0x84`) | 32 bytes: big-endian unsigned 256-bit integer. | $0 \dots 2^{256}-1$ | ✅ Yes |
| **`5`** | **`dec`** | `5` (`0x05`) | `133` (`0x85`) | 32 bytes: signed integer scaled by $10^{18}$ (`DECIMAL_SCALE`). | Signed 256-bit fixed point | ✅ Yes |
| **`6`** | *Reserved* | `6` (`0x06`) | N/A | Reserved for future protocol expansion. | Unused | ❌ No |
| **`7`** | **`bytes`** | `7` (`0x07`) | N/A | Raw unformatted binary bytes. | Up to 128 KiB (131,072 B) | ❌ **No (Never)** |
| **`8`** | **`str`** | `8` (`0x08`) | `136` (`0x88`) | UTF-8 encoded string. | Up to 128 bytes | ✅ Yes |
| **`9`** | **`addr`** | `9` (`0x09`) | `137` (`0x89`) | 32 bytes: Ethereum address right-aligned (12 zero bytes + 20 address bytes). | 20-byte address | ✅ Yes |
| **`10`**| **`key`** | `10` (`0x0A`)| `138` (`0x8A`) | 32 bytes: Raw 32-byte entity key. | 32 bytes | ✅ Yes |

> [!WARNING]
> Setting the indexed bit on `bytes` (`0x07 | 0x80 = 0x87`) is prohibited by the contract and reverts with `InvalidValueType(bytes32 name, uint8 typeId)`. Binary blobs are too large to maintain in query indexes.

---

## 5. Value Byte Serialization & Word Alignment

The `value` field is defined as dynamic Solidity `bytes`. Depending on the `typeId`, the payload must conform to precise binary layouts.

### 5.1 Single-Word 32-Byte Primitive Types

Seven types (`bool`, `i32`, `u64`, `u256`, `dec`, `addr`, `key`) are represented as **exactly 32 bytes** within the dynamic byte payload.

#### 1. `bool` (`typeId = 1 / 0x81`)
- Length: 32 bytes.
- False: `0x0000000000000000000000000000000000000000000000000000000000000000`
- True:  `0x0000000000000000000000000000000000000000000000000000000000000001`

#### 2. `i32` (`typeId = 2 / 0x82`)
- Length: 32 bytes.
- Sign-extended two's complement.
- Positive value $+42$:
  `0x000000000000000000000000000000000000000000000000000000000000002a`
- Negative value $-1$:
  `0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff`

#### 3. `u64` (`typeId = 3 / 0x83`)
- Length: 32 bytes.
- Value $1,700,000,000$ (`0x65541000`):
  `0x0000000000000000000000000000000000000000000000000000000065541000`

#### 4. `u256` (`typeId = 4 / 0x84`)
- Length: 32 bytes. Standard big-endian integer.

#### 5. `dec` (`typeId = 5 / 0x85`)
- Length: 32 bytes.
- Scaled by $10^{18}$ (`DECIMAL_SCALE = 1_000_000_000_000_000_000n`).
- Value `"4.5"` $= 4.5 \times 10^{18} = 4,500,000,000,000,000,000$ (`0x3e7104b2a8d60000`):
  `0x0000000000000000000000000000000000000000000000003e7104b2a8d60000`
- Value `"-2.1"` $= -2.1 \times 10^{18} = -2,100,000,000,000,000,000$ (in 256-bit two's complement):
  `0xffffffffffffffffffffffffffffffffffffffffffffffe2d7c04cd5950000`

#### 6. `addr` (`typeId = 9 / 0x89`)
- Length: 32 bytes.
- Address `0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045` right-aligned with 12 leading zero bytes:
  `0x000000000000000000000000d8da6bf26964af9d7eed9e03e53415d37aa96045`

#### 7. `key` (`typeId = 10 / 0x8A`)
- Length: 32 bytes. Exact 32-byte target entity key.

### 5.2 Variable-Length Types (`str`, `bytes`)

#### 1. `str` (`typeId = 8 / 0x88`)
- Format: UTF-8 encoded bytes.
- Maximum Length: **128 bytes** (`MAX_STRING_BYTES`).
- Example: `"Hello, Arkiv"` (12 bytes):
  `0x48656c6c6f2c2041726b6976`

#### 2. `bytes` (`typeId = 7`, Unindexed Only)
- Format: Raw binary octets.
- Individual Value Limit: **128 KiB** ($131,072$ bytes).
- Cumulative Operation Limit: The sum of all `bytes` values across all attributes in a single create or patch operation must not exceed **128 KiB** (`MAX_ENTITY_BYTES`).
- If total bytes exceeds $131,072$, execution reverts with `EntityTooLargeError`.

### 5.3 Tombstone Specification (Type 0)

The tombstone attribute is used exclusively inside Op 2 (`PATCH`) to remove an optional attribute:
- `name`: Target attribute name (e.g., `summary`).
- `typeId`: `0` (`0x00`).
- `value`: `0x` (0 bytes length).

#### Invariant Checks:
- If `typeId == 0` is passed to Op 1 (`CREATE`), reverts with `TombstoneInCreate(bytes32 name)`.
- If `typeId == 0` is passed with `value.length > 0`, reverts with `TombstoneValueNotEmpty(bytes32 name)`.

---

## 6. Canonical Attribute Ordering & Cardinality Invariants

### 6.1 Strict Lexicographical Ascending Sort

Every list of attributes submitted to the contract—whether in `CREATE` or `PATCH`—**must be sorted in strictly ascending byte order by `name`**:

$$\text{name}_0 < \text{name}_1 < \text{name}_2 < \dots < \text{name}_{n-1}$$

The comparison evaluates each `bytes32` as an unsigned 256-bit big-endian integer (or direct byte-by-byte lexicographical comparison):

```text
Attribute 0: "arkiv.schema" -> 0x61726b69762e736368656d6100...
Attribute 1: "author"       -> 0x617574686f7200000000000000...
Attribute 2: "category"     -> 0x63617465676f72790000000000...
Attribute 3: "score"        -> 0x73636f72650000000000000000...
Attribute 4: "title"        -> 0x7469746c650000000000000000...
```

If any $\text{name}_{i+1} \le \text{name}_i$, the contract immediately reverts with:

```solidity
revert AttributesNotSorted(); // Selector: 0x3206846a
```

### 6.2 Uniqueness Guarantee

Because the sort is **strictly** ascending ($<$), equal names ($\text{name}_{i+1} == \text{name}_i$) are forbidden. This prevents duplicate attributes from appearing in a single transaction.

### 6.3 Attribute Count Limits

- **Maximum Attributes Per Entity**: **32 attributes** (`MAX_ATTRIBUTES = 32`).
- **Named Schema Overhead**: When using a named schema, the `arkiv.schema` tag consumes one slot. Therefore, user schemas may define at most **31 user columns**.
- If `attributes.length > 32`, the contract reverts with:

```solidity
revert TooManyAttributes(uint256 count, uint256 maxCount); // Selector: 0xd7ce981c
```

---

## 7. Complete Calldata Byte Breakdown

Below is a complete annotated memory trace for an attribute list containing two attributes:
1. `category`: indexed `str` (`typeId = 0x88`), value `"news"`
2. `score`: indexed `dec` (`typeId = 0x85`), value $4.5 \times 10^{18}$

```text
Offset | Calldata Word Content                                             | Semantic Interpretation
-------|-------------------------------------------------------------------|-----------------------------------------------------
0x0000 | 0x0000000000000000000000000000000000000000000000000000000000000002 | Length of Attribute[] array = 2 elements
0x0020 | 0x0000000000000000000000000000000000000000000000000000000000000040 | Relative offset to Attribute[0] head (64 bytes)
0x0040 | 0x00000000000000000000000000000000000000000000000000000000000000c0 | Relative offset to Attribute[1] head (192 bytes)
-------|-------------------------------------------------------------------|-----------------------------------------------------
-- Attribute[0]: "category" (indexed str = 0x88, value "news")
0x0060 | 0x63617465676f7279000000000000000000000000000000000000000000000000 | Attribute[0].name: "category" left-aligned
0x0080 | 0x0000000000000000000000000000000000000000000000000000000000000088 | Attribute[0].typeId: 0x88 (Indexed str)
0x00a0 | 0x0000000000000000000000000000000000000000000000000000000000000060 | Relative offset to Attribute[0].value (96 bytes)
0x00c0 | 0x0000000000000000000000000000000000000000000000000000000000000004 | Length of Attribute[0].value = 4 bytes ("news")
0x00e0 | 0x6e65777300000000000000000000000000000000000000000000000000000000 | "news" left-aligned in 32-byte word
-------|-------------------------------------------------------------------|-----------------------------------------------------
-- Attribute[1]: "score" (indexed dec = 0x85, value 4.5e18)
0x0100 | 0x73636f7265000000000000000000000000000000000000000000000000000000 | Attribute[1].name: "score" left-aligned
0x0120 | 0x0000000000000000000000000000000000000000000000000000000000000085 | Attribute[1].typeId: 0x85 (Indexed dec)
0x0140 | 0x0000000000000000000000000000000000000000000000000000000000000060 | Relative offset to Attribute[1].value (96 bytes)
0x0160 | 0x0000000000000000000000000000000000000000000000000000000000000020 | Length of Attribute[1].value = 32 bytes
0x0180 | 0x0000000000000000000000000000000000000000000000003e7104b2a8d60000 | Scaled decimal value 4.5 * 10^18
```

---

## 8. Reference Serialization Implementations

### 8.1 TypeScript Sorting & Encoding Utility

```typescript
import { type Hex, padHex, stringToHex, toHex } from "viem";

export const INDEXED_FLAG = 0x80;
export const MAX_ATTRIBUTES = 32;

export interface RawAttribute {
  name: string;
  typeId: number;
  value: Hex;
}

export interface EncodedAttribute {
  name: Hex;
  typeId: number;
  value: Hex;
}

/**
 * Packs an ASCII name into a left-aligned, zero-padded 32-byte hex string
 */
export function packAttributeName(name: string): Hex {
  if (name.length === 0) {
    throw new Error("Ident32Empty: attribute name cannot be empty");
  }
  if (name.startsWith("$")) {
    throw new Error(`SystemAttributeNotWritable: ${name} cannot start with $`);
  }
  if (name.length > 32) {
    throw new Error(`Attribute name ${name} exceeds 32 bytes`);
  }
  // Convert ASCII string to hex and right-pad with zeroes to 32 bytes (64 hex characters + 0x)
  return padHex(stringToHex(name), { size: 32, dir: "right" });
}

/**
 * Validates, serializes, and sorts an attribute list according to protocol invariants
 */
export function prepareAttributes(attrs: RawAttribute[]): EncodedAttribute[] {
  if (attrs.length > MAX_ATTRIBUTES) {
    throw new Error(`TooManyAttributes: ${attrs.length} > ${MAX_ATTRIBUTES}`);
  }

  const encoded: EncodedAttribute[] = attrs.map((a) => ({
    name: packAttributeName(a.name),
    typeId: a.typeId,
    value: a.value,
  }));

  // Sort strictly ascending by name byte value
  encoded.sort((a, b) => {
    if (a.name < b.name) return -1;
    if (a.name > b.name) return 1;
    throw new Error(`DuplicateAttributeError: duplicate attribute name ${a.name}`);
  });

  return encoded;
}
```
