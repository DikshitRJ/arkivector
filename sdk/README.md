# Arkivector SDKs

Client libraries for [Arkivector](https://github.com/arkivector/arkivector) — the vector co-processor for Arkiv.

## Available Packages

| Language | Package | Status | Documentation |
| :--- | :--- | :---: | :--- |
| **Python** | `arkivector` | Active (v0.1.0) | [`docs/api/sdk-guidelines.md#python`](../docs/api/sdk-guidelines.md#python-sdkpython-package-arkivector-python--39-stdlib-only) |
| **Node.js** | `@dikshitrj/arkivector` | Active (v0.1.0) | [`docs/api/sdk-guidelines.md#node`](../docs/api/sdk-guidelines.md#nodejs--typescript-sdknode-package-arkivectorclient-node--18-zero-runtime-deps-esm) |
| **C** | `libarkivector` | *(Deferred)* | [`docs/api/sdk-guidelines.md#c`](../docs/api/sdk-guidelines.md#c-sdkc-library-libarkivector-c11-deps-libcurl--vendored-yyjson) |
| **C++** | `arkivector-cpp` | *(Deferred)* | [`docs/api/sdk-guidelines.md#cpp`](../docs/api/sdk-guidelines.md#c-sdkcpp-c17-wraps-the-c-sdk) |
| **Rust** | `arkivector` | *(Deferred)* | [`docs/api/sdk-guidelines.md#rust`](../docs/api/sdk-guidelines.md#rust-sdkrust-crate-arkivector-msrv-175) |

---

## Installation

### Python (Python ≥ 3.9)
```bash
pip install arkivector
```
Optional extras:
```bash
pip install arkivector[numpy]
pip install arkivector[langchain]
pip install arkivector[llama-index]
```

### Node.js (Node ≥ 18)
```bash
npm install @dikshitrj/arkivector
```
Optional peer dependency for LangChain.js:
```bash
npm install @langchain/core
```

---

## API Contract & Documentation

- [SDK Guidelines & Per-Language Surface](../docs/api/sdk-guidelines.md)
- [Conventions (Headers, Retries, Timeouts)](../docs/api/conventions.md)
- [Vector Formats & VectorBytes v1 Specification](../docs/api/vectors.md)
- [Error Codes & Hierarchy](../docs/api/errors.md)
- [Filter AST & Arkiv Query Language](../docs/api/filters.md)
- [Search Operations & Options](../docs/api/search.md)
- [Entities, Direct Writes & Upsert](../docs/api/entities.md)
- [Signing Writes With Your Own Wallet (prepare + submit)](../docs/wiki/getting-started/signing.md)
- [Collections Management](../docs/api/collections.md)
- [OpenAPI Specification](../spec/openapi.yaml)
