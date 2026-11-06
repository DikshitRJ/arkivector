# `daemon/src/sync/discovery.c`

Implements discovery strategies B and C for open collections.

## Overview
- **Strategy B (Calldata Decoding)**: Decodes EVM transactions targeting the Arkiv contract to find `execute((uint8,bytes)[])` calls. Matches decoded operation logs to extract schema tags and attributes without issuing extra JSON-RPC queries.
- **Strategy C (On-Demand Probing)**: When internal transactions or contract interactions omit calldata on the top-level transaction, executes targeted probe queries (`arkiv.schema = str(tag) AND ($key = ... OR ...)`).
- **Not-Mine Cache**: Caches non-matching keys to avoid re-querying dead keys across multiple sync passes.
