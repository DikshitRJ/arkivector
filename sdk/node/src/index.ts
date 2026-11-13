/**
 * Arkivector Node.js / TypeScript SDK
 * Docs: docs/api/README.md
 */

export const SUPPORTED_API_MAJOR = 1;
export const VERSION = "0.1.0";

// Error hierarchy
export * from "./errors.js";

// Vector types
export * from "./types.js";

// VectorBytes codec
export * from "./vectorbytes.js";

// Filter builder F and filter types
export * from "./filter.js";

// Models, TypedValue, T and Ttl helpers
export * from "./models.js";

// HTTP transport and helpers
export * from "./http.js";

// Client-side signing helpers (EIP-1193 / viem / ethers converters)
export * from "./signing.js";

// Client
export * from "./client.js";
