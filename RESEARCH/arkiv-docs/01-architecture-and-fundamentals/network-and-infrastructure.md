# Arkiv Network Topography, Infrastructure & Authentication

## Table of Contents
- [1. Executive Summary](#1-executive-summary)
- [2. The Rabdi Public Testnet](#2-the-rabdi-public-testnet)
  - [2.1. Network Parameters & Identity](#21-network-parameters--identity)
  - [2.2. Endpoints & Explorer](#22-endpoints--explorer)
  - [2.3. Native Gas Token (`tGLM`) & Faucet](#23-native-gas-token-tglm--faucet)
- [3. Node Architecture & Dual Interface](#3-node-architecture--dual-interface)
  - [3.1. EVM Transaction Processing (`0x44..44`)](#31-evm-transaction-processing-0x4444)
  - [3.2. Dedicated `arkiv_*` JSON-RPC Methods](#32-dedicated-arkiv_-json-rpc-methods)
- [4. Authentication & Access Key Infrastructure](#4-authentication--access-key-infrastructure)
  - [4.1. The Arkiv Hub Gateway](#41-the-arkiv-hub-gateway)
  - [4.2. Access Key Lifecycle & Scope](#42-access-key-lifecycle--scope)
  - [4.3. Quota System: Cost Units (CU)](#43-quota-system-cost-units-cu)
  - [4.4. The Three Transport Authentication Modes](#44-the-three-transport-authentication-modes)
    - [Mode 1: URL Path Parameter](#mode-1-url-path-parameter)
    - [Mode 2: `X-API-KEY` HTTP Header](#mode-2-x-api-key-http-header)
    - [Mode 3: `Authorization: Bearer` Header](#mode-3-authorization-bearer-header)
    - [WebSocket Connection Authentication](#websocket-connection-authentication)
- [5. Security Hardening & Key Lockdown](#5-security-hardening--key-lockdown)
  - [5.1. Origins Allowlist (Browser Attribution)](#51-origins-allowlist-browser-attribution)
  - [5.2. IP Allowlist (Server Access Control)](#52-ip-allowlist-server-access-control)
  - [5.3. Key Rotation & Revocation](#53-key-rotation--revocation)
  - [5.4. Critical Pitfall: In-Wallet RPC Key Leakage](#54-critical-pitfall-in-wallet-rpc-key-leakage)
- [6. Connecting with the TypeScript SDK](#6-connecting-with-the-typescript-sdk)
  - [6.1. Public Client Initialization](#61-public-client-initialization)
  - [6.2. Server-Side Wallet Client](#62-server-side-wallet-client)
  - [6.3. Browser Wallet Integration (Wagmi / Injected)](#63-browser-wallet-integration-wagmi--injected)
  - [6.4. Error Handling (`MissingAccessKeyError`, HTTP 401)](#64-error-handling-missingaccesskeyerror-http-401)
- [7. Historical Networks & Protocol Evolution](#7-historical-networks--protocol-evolution)
  - [7.1. Lineage Matrix (Kaolin -> Braga -> Tiramisu -> Rabdi)](#71-lineage-matrix-kaolin---braga---tiramisu---rabdi)
  - [7.2. Critical Breaking Changes from Tiramisu to Rabdi](#72-critical-breaking-changes-from-tiramisu-to-rabdi)
- [8. Conclusion & Reference Links](#8-conclusion--reference-links)

---

## 1. Executive Summary

Connecting to Arkiv requires interfacing with a specialized network topography. While Arkiv executes consensus and commits state through standard EVM infrastructure, its RPC nodes are augmented with high-throughput query and indexing sub-engines.

To safeguard validating nodes against denial-of-service (DoS) attacks and abusive load, traffic to Arkiv's public endpoints passes through the **Arkiv Hub Gateway**, which enforces **Access Key Authentication** and tracks resource consumption in **Cost Units (CU)**. This document specifies the complete network topology, endpoint parameters, authentication mechanisms, and historical protocol evolutions.

---

## 2. The Rabdi Public Testnet

**Rabdi** is Arkiv's primary active public testnet. It provides developers with an environment to test smart contract transactions, evaluate query latencies, and stress-test data models against live consensus.

### 2.1. Network Parameters & Identity

| Parameter | Specification | Notes |
|---|---|---|
| **Network Name** | `Arkiv Rabdi Testnet` | Current active public testnet |
| **Chain ID (Decimal)** | `7794095` | EIP-155 Chain ID |
| **Chain ID (Hexadecimal)** | `0x76edaf` | Hex encoding in RPC / MetaMask |
| **Canonical Arkiv Address** | `0x4400000000000000000000000000000000000044` | Identical across all Arkiv networks |
| **Block Time** | `2.0 seconds` | Discrete block generation cadence |
| **Consensus Engine** | EVM-compatible consensus | State committed via Keccak MPT |
| **Gas Token Symbol** | `tGLM` | Test Golem Network Token |

### 2.2. Endpoints & Explorer

```
+-----------------------------------------------------------------------------------------------+
|                                    RABDI ENDPOINT DIRECTORY                                   |
+-------------------+---------------------------------------------------------------------------+
| Interface         | URL / URI                                                                 |
+-------------------+---------------------------------------------------------------------------+
| HTTP JSON-RPC     | https://rpc.rabdi.db-chain.testnet.arkiv.network                          |
| WebSocket RPC     | wss://rpc.rabdi.db-chain.testnet.arkiv.network                            |
| Arkiv Hub (Portal)| https://hub.arkiv.network                                                 |
| Access Key Manager| https://hub.arkiv.network/access-keys                                     |
| Public Faucet     | https://hub.arkiv.network/faucet                                          |
| Block Explorer    | https://rabdi.explorer.arkiv.network                                      |
+-------------------+---------------------------------------------------------------------------+
```

### 2.3. Native Gas Token (`tGLM`) & Faucet

All on-chain writes (creating, patching, extending, transferring, and deleting entities) consume gas priced in `tGLM`:
1. Developers fund their accounts via the [Arkiv Hub Faucet](https://hub.arkiv.network/faucet).
2. The faucet distributes `tGLM` to verified wallet addresses.
3. Transaction value is always zero (`value = 0`). Only EVM execution and storage rent fees are deducted from the sender's balance.

---

## 3. Node Architecture & Dual Interface

```mermaid
flowchart TD
    subgraph Client ["Client Layer"]
        C1["JSON-RPC Client (cURL / Viem)"]
        C2["Arkiv TypeScript SDK"]
    end

    subgraph HubGateway ["Arkiv Hub Layer"]
        Gateway["Rate Limiter & Quota Gatekeeper"]
        AuthValidator{"Valid Access Key?<br/>Origin / IP Allowed?"}
    end

    subgraph RabdiNode ["Arkiv Validator Node"]
        HTTPRouter["HTTP / WS Router"]
        StdEVM["Standard EVM Handler<br/>(eth_*, net_*, web3_*)"]
        ArkivEngine["Arkiv Index & Query Engine<br/>(arkiv_query, arkiv_getEntity, arkiv_getBlockTiming)"]
        Precompile["Arkiv Core Precompile<br/>0x44..44 execute()"]
        MPT["Keccak Merkle-Patricia Trie"]
    end

    C1 --> Gateway
    C2 --> Gateway
    Gateway --> AuthValidator
    AuthValidator -- Yes --> HTTPRouter
    AuthValidator -- No --> Err401["HTTP 401 Unauthorized / Rate Limited"]

    HTTPRouter -->|eth_sendRawTransaction| StdEVM
    HTTPRouter -->|eth_call (entityNonce)| Precompile
    HTTPRouter -->|arkiv_* Read Methods| ArkivEngine
    StdEVM --> Precompile
    Precompile --> MPT
    MPT --> ArkivEngine
```

### 3.1. EVM Transaction Processing (`0x44..44`)
- Mutations are not handled via custom JSON-RPC methods. Instead, they are standard Ethereum transactions dispatched to the precompile/contract address `0x4400000000000000000000000000000000000044`.
- Standard methods supported include:
  - `eth_sendRawTransaction`: Broadcasts encoded batch operations calling `execute((uint8, bytes)[])`.
  - `eth_call`: Reads state without gas, such as reading an owner's entity counter via `entityNonce(address)` (`0x36917bfd`).
  - `eth_estimateGas`: Simulates execution to calculate write rent and gas limits.
  - `eth_getLogs` / `eth_subscribe`: Subscribes to real-time events emitted by the Arkiv address (`EntityCreated`, `EntityPatched`, `ExpiryExtended`, etc.).

### 3.2. Dedicated `arkiv_*` JSON-RPC Methods
Read operations bypass the EVM state interpreter and execute directly against the node's native index structures:
- `arkiv_query(queryText, options)`: Evaluates AST predicates, runs projections, and returns matching entities.
- `arkiv_getEntity(entityKey)`: Direct $O(1)$ key-based lookup.
- `arkiv_getBlockTiming()`: Returns latest block number, timestamp, and duration.

---

## 4. Authentication & Access Key Infrastructure

### 4.1. The Arkiv Hub Gateway

Arkiv's raw public RPC endpoints are heavily throttled to prevent resource exhaustion and Sybil spam. Production dApps and developers obtain an **Access Key** from the [Arkiv Hub](https://hub.arkiv.network/access-keys).

- **Public RPC without Key**: Passing `accessKey: ""` allows requests to proceed through a heavily restricted rate-limiter, logging client-side warnings.
- **RPC with Valid Key**: Grants access to full throughput and allocates a dedicated monthly quota.

### 4.2. Access Key Lifecycle & Scope

1. **One Key Per Wallet Per Network**: A single wallet can hold at most one active key for Rabdi.
2. **Network Scoping**: An access key is cryptographically bound to the network selected at generation (e.g. Rabdi). Keys cannot be shared across multiple networks.
3. **No Financial Risk**: An access key is **not a private key**. It cannot sign transactions or drain funds. However, anyone who possesses your key can consume your monthly Cost Unit quota.

### 4.3. Quota System: Cost Units (CU)

The Arkiv Hub tracks usage through **Cost Units (CU)**:
- Different JSON-RPC methods consume different CU amounts based on computational complexity (e.g. `arkiv_query` with multiple joins or large limits consumes more CU than `eth_blockNumber`).
- The **Usage** tab on the Hub monitors consumed CU, total requests made, and quota reset timestamps.
- When the quota is exhausted, subsequent requests receive HTTP status `429 Too Many Requests` or RPC-level errors until the billing cycle resets or a quota extension is granted.

### 4.4. The Three Transport Authentication Modes

The Arkiv RPC infrastructure accepts access keys in three standardized formats across both HTTP and WebSocket transports:

```
+-----------------------------------------------------------------------------------------------+
|                               ACCESS KEY AUTHENTICATION METHODS                               |
+---------------------+-------------------------------------------------------------------------+
| Format              | Implementation Example                                                  |
+---------------------+-------------------------------------------------------------------------+
| URL Path Parameter  | https://rpc.rabdi.db-chain.testnet.arkiv.network/<YOUR_KEY>              |
| X-API-KEY Header    | X-API-KEY: <YOUR_KEY>                                                   |
| Bearer Token Header | Authorization: Bearer <YOUR_KEY>                                        |
+---------------------+-------------------------------------------------------------------------+
```

#### Mode 1: URL Path Parameter
The access key is appended as the terminal path segment of the RPC URL. This is the default format produced by the SDK's `rabdi({ accessKey })` chain factory:

```bash
curl "https://rpc.rabdi.db-chain.testnet.arkiv.network/ak_live_abcdef123456" \
  -H "content-type: application/json" \
  --data '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}'
```

#### Mode 2: `X-API-KEY` HTTP Header
The key is supplied as a custom HTTP header:

```bash
curl "https://rpc.rabdi.db-chain.testnet.arkiv.network" \
  -H "content-type: application/json" \
  -H "X-API-KEY: ak_live_abcdef123456" \
  --data '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}'
```

#### Mode 3: `Authorization: Bearer` Header
The key is supplied using standard HTTP Bearer token authorization:

```bash
curl "https://rpc.rabdi.db-chain.testnet.arkiv.network" \
  -H "content-type: application/json" \
  -H "Authorization: Bearer ak_live_abcdef123456" \
  --data '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}'
```

#### WebSocket Connection Authentication
WebSockets support either the URL path or headers:
```bash
wscat -c "wss://rpc.rabdi.db-chain.testnet.arkiv.network/ak_live_abcdef123456" \
  -x '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}' \
  -w 1
```

---

## 5. Security Hardening & Key Lockdown

Because access keys embedded in client-side single-page applications (React, Vue, mobile apps) are publicly discoverable via browser developer tools, the Arkiv Hub provides two lockdown firewalls.

```
+-----------------------------------------------------------------------------------------------+
|                                   ACCESS KEY ACCESS CONTROL                                   |
|                                                                                               |
|   Inbound Request                                                                             |
|         |                                                                                     |
|         +---> Browser Request? ----> Check Origins Allowlist -----> [Match Domain or Reject]  |
|         |                                                                                     |
|         +---> Server Request? -----> Check IP Allowlist (No CIDR) -> [Match IP or Reject]     |
+-----------------------------------------------------------------------------------------------+
```

### 5.1. Origins Allowlist (Browser Attribution)
- Developers can configure a list of authorized origin domains (e.g., `https://myapp.com`, `https://staging.myapp.com`).
- The Hub gateway inspects the incoming HTTP `Origin` header.
- **Security Note**: Non-browser clients (such as cURL or Python scripts) can spoof the `Origin` header. Thus, the Origins allowlist serves as browser-level attribution and casual abuse deterrence, not absolute cryptographic access control.

### 5.2. IP Allowlist (Server Access Control)
- For backend daemons, indexers, or API servers, developers restrict access to specific static IP addresses.
- **Strict Format**: The allowlist requires exact, discrete IPv4 or IPv6 addresses. **CIDR subnet ranges are not supported.**
- Incoming requests from any unlisted IP address are immediately rejected at the gateway. This provides enterprise-grade access control for backend infrastructure.

### 5.3. Key Rotation & Revocation

The Arkiv Hub UI provides two key management operations:
- **Regenerate Key**: Generates a new cryptographic key string and **immediately deactivates the previous key**. Active services using the old key will begin failing with HTTP 401 until updated.
- **Delete Key**: Permanently deletes the access key. Applications can create a fresh key subsequently.

### 5.4. Critical Pitfall: In-Wallet RPC Key Leakage

> [!CAUTION]
> **NEVER add an access-key-authenticated RPC URL to an end-user's browser wallet (MetaMask, Coinbase Wallet, Rainbow)!**

When a dApp prompts a user to switch or add Rabdi using `walletClient.addChain`, browser wallets cache the provided RPC URL and reuse it across **every subsequent interaction and third-party application the user visits**. If your private access key is embedded in that URL, the user's wallet will broadcast all future transactions through your key, rapidly draining your monthly Cost Unit quota.

#### The Safe Pattern:
Always initialize the user's wallet chain **without** an access key:

```ts
import { rabdi } from "@arkiv-network/sdk/chains"

// 1. App's internal client (Public/Backend) uses private access key
const appClient = createPublicClient({
  chain: rabdi({ accessKey: process.env.ARKIV_ACCESS_KEY }),
  transport: http(),
})

// 2. User's wallet is configured WITHOUT access key
await walletClient.addChain({
  chain: rabdi({ accessKey: "" }), // Uses unauthenticated public RPC
})
```

---

## 6. Connecting with the TypeScript SDK

### 6.1. Public Client Initialization

```ts
import { createPublicClient } from "@arkiv-network/sdk"
import { rabdi } from "@arkiv-network/sdk/chains"
import { http } from "viem"

// Initializing the chain definition with the access key
const chain = rabdi({ accessKey: process.env.ARKIV_ACCESS_KEY ?? "" })

// Creating a read-only public client
export const publicClient = createPublicClient({
  chain,
  transport: http(), // http() with no URL automatically inherits chain.rpcUrls.default.http[0]
})
```

### 6.2. Server-Side Wallet Client

```ts
import { createWalletClient } from "@arkiv-network/sdk"
import { rabdi } from "@arkiv-network/sdk/chains"
import { http, type Hex } from "viem"
import { privateKeyToAccount } from "viem/accounts"

const chain = rabdi({ accessKey: process.env.ARKIV_ACCESS_KEY ?? "" })
const account = privateKeyToAccount(process.env.PRIVATE_KEY as Hex)

export const serverWalletClient = createWalletClient({
  chain,
  transport: http(),
  account,
})
```

### 6.3. Browser Wallet Integration (Wagmi / Injected)

```ts
import { createWalletClient } from "@arkiv-network/sdk"
import { rabdi } from "@arkiv-network/sdk/chains"
import { custom } from "viem"

// Request accounts from injected window.ethereum
const [userAddress] = await window.ethereum.request({
  method: "eth_requestAccounts",
})

export const browserWalletClient = createWalletClient({
  chain: rabdi({ accessKey: "" }), // No key in browser wallet!
  transport: custom(window.ethereum),
  account: userAddress,
})
```

### 6.4. Error Handling (`MissingAccessKeyError`, HTTP 401)

- **`MissingAccessKeyError`**: Thrown client-side if `rabdi()` is invoked with `undefined` or invalid options.
- **HTTP 401 Unauthorized**: Returned by the gateway if the key string is invalid, revoked, or unrecognized.
- **HTTP 429 Too Many Requests**: Returned if the key exceeds its monthly Cost Unit quota or bursts past instantaneous rate limits.

---

## 7. Historical Networks & Protocol Evolution

Arkiv's consensus and query architecture underwent several major evolutionary iterations prior to Rabdi.

### 7.1. Lineage Matrix (Kaolin -> Braga -> Tiramisu -> Rabdi)

| Network | Status | Chain ID (Dec / Hex) | Native Gas Token | Retirement Date |
|---|---|---|---|---|
| **Kaolin** | **Retired** | `60138453025` / `0xe0087f821` | ETH | May 15, 2026 |
| **Braga** | **Retired** | `60138453102` / `0xe0087f86e` | GLM | August 12, 2026 |
| **Tiramisu**| **Replaced**| `7738577` / `0x7614d1` | GLM | Late 2026 |
| **Rabdi** | **ACTIVE** | `7794095` / `0x76edaf` | `tGLM` | Current Public Testnet |

> [!NOTE]
> State is **never** ported across testnets. Entities created on Tiramisu, Braga, or Kaolin do not exist on Rabdi.

### 7.2. Critical Breaking Changes from Tiramisu to Rabdi

Migrating existing applications from Tiramisu to Rabdi requires adapting to 5 major protocol changes:

1. **Mandatory Access Keys**: `rabdi` is a constructor function `rabdi({ accessKey })`. Passing no key falls back to the rate-limited public RPC.
2. **Deprecation of `not` and `ne`**: Rabdi's query engine dropped support for inequality (`!=` / `ne`) and logical negation (`not`) to ensure predictable index scans and prevent expensive unindexed table traversals. Applications must use positive range or equality filters instead.
3. **Removal of `atBlock` Queries**: Rabdi prunes historical state past the current block horizon. All queries execute strictly against the **latest block**. Querying at historical blocks (`atBlock: 12345`) is unsupported.
4. **Pagination Streamlined to `offset` and Key Cursors**: Legacy iterator constructs (`next()`, `hasNextPage()`, `for await`) were removed in favor of explicit `limit` / `offset` and key-cursor pagination (`where(gt("$key", key(lastKey)))`).
5. **Real-time Event Watchers**: Log subscriptions and event watchers on Rabdi do not support `fromBlock` in the past. Watchers stream events strictly from the block at which subscription commences.

---

## 8. Conclusion & Reference Links

Rabdi represents Arkiv's hardened, production-ready testnet foundation. By routing connections through the Arkiv Hub Gateway with access key quotas and IP/Origin allowlists, Arkiv achieves high-throughput querying while preserving the decentralization and consensus guarantees of the underlying EVM state trie.

### Related Architecture Modules
- [Arkiv Overview & Four Principles](file:///home/dikshitrj/Coding/arkivector/RESEARCH/arkiv-docs/01-architecture-and-fundamentals/overview.md)
- [Entity Model & System Fields](file:///home/dikshitrj/Coding/arkivector/RESEARCH/arkiv-docs/01-architecture-and-fundamentals/entity-model.md)
- [Attributes, Types & Indexing Mechanics](file:///home/dikshitrj/Coding/arkivector/RESEARCH/arkiv-docs/01-architecture-and-fundamentals/attributes-and-types.md)
- [Lifecycle, Block Timing & Expiration](file:///home/dikshitrj/Coding/arkivector/RESEARCH/arkiv-docs/01-architecture-and-fundamentals/lifecycle-and-expiration.md)
