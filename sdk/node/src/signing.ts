/**
 * Client-side signing (contract 1.1.0, GitHub issue #12). Zero dependencies and no Node built-ins,
 * so these converters also work in a browser bundle.
 *
 * Flow: `client.prepare()` builds unsigned EIP-1559 transactions → you sign them with any wallet →
 * `client.submit()` hands them back (raw signed transactions, or the hashes a wallet broadcast).
 * `client.upsertSigned()` does all three with any of the signers below.
 *
 * Supported signers (duck-typed, nothing is imported):
 * - EIP-1193 provider (MetaMask `window.ethereum`, WalletConnect, Rabby, Frame, hardware wallets through
 *   those): the wallet signs and broadcasts with `eth_sendTransaction`; the daemon waits for the hashes.
 * - viem `WalletClient`: local accounts sign (`signTransaction`), JSON-RPC accounts send.
 * - ethers `Signer` (v6): `signTransaction`, falling back to `sendTransaction` (browser signers).
 * - A function `(tx) => rawSignedTx | txHash` (hex string or bytes), e.g. a KMS call.
 * - An object with `signTransaction(tx)` (returns raw) or `sendTransaction(tx)` (returns a hash).
 * Docs: docs/api/entities.md §4, docs/wiki/getting-started/signing.md
 */
import type { PreparedTransaction } from "./models.js";

/** EIP-1193 provider (window.ethereum and friends). */
export interface Eip1193Provider {
  request(args: { method: string; params?: unknown }): Promise<unknown>;
}

/** The parts of a viem WalletClient this SDK uses. */
export interface ViemWalletClientLike {
  account?: { address: string; type?: string } | undefined;
  getAddresses?: () => Promise<readonly string[]>;
  signTransaction?: (args: Record<string, unknown>) => Promise<string>;
  sendTransaction: (args: Record<string, unknown>) => Promise<string>;
}

/** The parts of an ethers v6 Signer this SDK uses. */
export interface EthersSignerLike {
  getAddress(): Promise<string>;
  signTransaction?: (tx: Record<string, unknown>) => Promise<string>;
  sendTransaction(tx: Record<string, unknown>): Promise<{ hash: string }>;
}

/** Returns a raw signed transaction (0x02… hex or bytes) or, if it broadcast itself, a tx hash. */
export type SignFunction = (
  tx: PreparedTransaction,
) => Promise<string | Uint8Array> | string | Uint8Array;

export type TransactionSigner =
  | Eip1193Provider
  | ViemWalletClientLike
  | EthersSignerLike
  | SignFunction
  | { signTransaction(tx: PreparedTransaction): Promise<string | Uint8Array> }
  | { sendTransaction(tx: PreparedTransaction): Promise<string> };

/** One signed transaction: raw bytes for the daemon to broadcast, or a hash the wallet broadcast. */
export type SignedOutput =
  { kind: "raw"; value: string } | { kind: "hash"; value: string };

const HASH_RE = /^0x[0-9a-fA-F]{64}$/;
const RAW_RE = /^0x02(?:[0-9a-fA-F]{2})+$/;

function need<T>(v: T | null | undefined, what: string): T {
  if (v === null || v === undefined) {
    throw new Error(
      `prepared transaction lacks ${what} (daemon older than contract 1.1.0); set it before signing`,
    );
  }
  return v;
}

function hexQty(n: number | bigint): string {
  return "0x" + BigInt(n).toString(16);
}

/** Bytes or hex text → lowercase 0x-hex. */
export function toHex(v: string | Uint8Array): string {
  if (typeof v === "string") {
    const s = v.startsWith("0x") || v.startsWith("0X") ? v.slice(2) : v;
    return "0x" + s.toLowerCase();
  }
  let out = "0x";
  for (const b of v) out += b.toString(16).padStart(2, "0");
  return out;
}

/**
 * JSON-RPC transaction object (hex quantities) for `eth_sendTransaction` / `eth_signTransaction`:
 * what EIP-1193 wallets such as MetaMask expect. Plain JSON: send it to a frontend as is.
 */
export function toRpcTransaction(
  tx: PreparedTransaction,
): Record<string, string> {
  const out: Record<string, string> = {
    type: "0x2",
    chainId: hexQty(need(tx.chainId, "chainId")),
    nonce: hexQty(tx.nonce),
    to: tx.to,
    value: tx.value,
    data: tx.data,
    gas: hexQty(need(tx.gas, "gas")),
    maxFeePerGas: need(tx.maxFeePerGas, "maxFeePerGas"),
    maxPriorityFeePerGas: need(tx.maxPriorityFeePerGas, "maxPriorityFeePerGas"),
  };
  if (tx.from) out.from = tx.from;
  return out;
}

/** viem transaction request (bigint quantities); pass to `signTransaction` / `sendTransaction`. */
export function toViemTransaction(
  tx: PreparedTransaction,
): Record<string, unknown> {
  return {
    type: "eip1559",
    chainId: need(tx.chainId, "chainId"),
    nonce: tx.nonce,
    to: tx.to,
    value: BigInt(tx.value),
    data: tx.data,
    gas: BigInt(need(tx.gas, "gas")),
    maxFeePerGas: BigInt(need(tx.maxFeePerGas, "maxFeePerGas")),
    maxPriorityFeePerGas: BigInt(
      need(tx.maxPriorityFeePerGas, "maxPriorityFeePerGas"),
    ),
  };
}

/** ethers v6 TransactionRequest (bigint quantities). */
export function toEthersTransaction(
  tx: PreparedTransaction,
): Record<string, unknown> {
  const out: Record<string, unknown> = {
    type: 2,
    chainId: BigInt(need(tx.chainId, "chainId")),
    nonce: tx.nonce,
    to: tx.to,
    value: BigInt(tx.value),
    data: tx.data,
    gasLimit: BigInt(need(tx.gas, "gas")),
    maxFeePerGas: BigInt(need(tx.maxFeePerGas, "maxFeePerGas")),
    maxPriorityFeePerGas: BigInt(
      need(tx.maxPriorityFeePerGas, "maxPriorityFeePerGas"),
    ),
  };
  if (tx.from) out.from = tx.from;
  return out;
}

/** Classify a signer's answer: a 32-byte hash means it broadcast; 0x02… means raw signed bytes. */
export function classifySigned(v: string | Uint8Array): SignedOutput {
  const h = toHex(v);
  if (typeof v === "string" && HASH_RE.test(h))
    return { kind: "hash", value: h };
  if (RAW_RE.test(h)) return { kind: "raw", value: h };
  throw new Error(
    "signer returned neither a signed EIP-1559 (0x02…) transaction nor a transaction hash",
  );
}

function isFn(v: unknown): v is (...a: unknown[]) => unknown {
  return typeof v === "function";
}

function errorCode(e: unknown): number | undefined {
  if (e && typeof e === "object" && "code" in e) {
    const c = (e as { code: unknown }).code;
    return typeof c === "number" ? c : undefined;
  }
  return undefined;
}

/** A resolved signer: its address (when it knows one) and a per-transaction sign/send step. */
export interface ResolvedSigner {
  kind: "eip1193" | "viem" | "ethers" | "function" | "object";
  address(): Promise<string | null>;
  /** Called once before signing with the transactions' chain id (wallet chain switching). */
  prepareChain(chainId: number): Promise<void>;
  sign(tx: PreparedTransaction): Promise<SignedOutput>;
}

/** Detect which kind of signer this is (see the module comment). */
export function resolveSigner(signer: TransactionSigner): ResolvedSigner {
  if (isFn(signer)) {
    const fn = signer as SignFunction;
    return {
      kind: "function",
      address: async () => null,
      prepareChain: async () => {},
      sign: async (tx) => classifySigned(await fn(tx)),
    };
  }
  const o = signer as unknown as Record<string, unknown>;
  // ethers Signer: getAddress + sendTransaction.
  if (isFn(o.getAddress) && isFn(o.sendTransaction)) {
    const s = signer as EthersSignerLike;
    return {
      kind: "ethers",
      address: () => s.getAddress(),
      prepareChain: async () => {},
      sign: async (tx) => {
        const req = toEthersTransaction(tx);
        if (isFn(s.signTransaction)) {
          try {
            return { kind: "raw", value: toHex(await s.signTransaction(req)) };
          } catch {
            // Browser (JSON-RPC) signers cannot sign without sending: fall through.
          }
        }
        const sent = await s.sendTransaction(req);
        return { kind: "hash", value: toHex(sent.hash) };
      },
    };
  }
  // viem WalletClient: sendTransaction + account/getAddresses.
  if (isFn(o.sendTransaction) && ("account" in o || isFn(o.getAddresses))) {
    const w = signer as ViemWalletClientLike;
    return {
      kind: "viem",
      address: async () => {
        if (w.account?.address) return w.account.address;
        const a = w.getAddresses ? await w.getAddresses() : [];
        return a[0] ?? null;
      },
      prepareChain: async () => {},
      sign: async (tx) => {
        const req = {
          ...toViemTransaction(tx),
          account: w.account ?? tx.from,
          chain: null,
        };
        if (w.account?.type === "local" && isFn(w.signTransaction)) {
          return { kind: "raw", value: toHex(await w.signTransaction(req)) };
        }
        return { kind: "hash", value: toHex(await w.sendTransaction(req)) };
      },
    };
  }
  // EIP-1193 provider.
  if (isFn(o.request)) {
    const p = signer as Eip1193Provider;
    return {
      kind: "eip1193",
      address: async () => {
        let accts = (await p.request({ method: "eth_accounts" })) as string[];
        if (!Array.isArray(accts) || accts.length === 0) {
          accts = (await p.request({
            method: "eth_requestAccounts",
          })) as string[];
        }
        return Array.isArray(accts) && accts.length > 0 ? accts[0] : null;
      },
      prepareChain: async (chainId) => {
        const current = (await p.request({ method: "eth_chainId" })) as string;
        if (BigInt(current) === BigInt(chainId)) return;
        try {
          await p.request({
            method: "wallet_switchEthereumChain",
            params: [{ chainId: hexQty(chainId) }],
          });
        } catch (e) {
          if (errorCode(e) === 4902) {
            throw new Error(
              `the wallet does not know Arkiv chain ${chainId}: add it first (wallet_addEthereumChain ` +
                `with the Arkiv RPC URL), then retry`,
            );
          }
          throw e;
        }
      },
      sign: async (tx) => {
        const hash = (await p.request({
          method: "eth_sendTransaction",
          params: [toRpcTransaction(tx)],
        })) as string;
        return { kind: "hash", value: toHex(hash) };
      },
    };
  }
  if (isFn(o.signTransaction)) {
    const s = signer as {
      signTransaction(tx: PreparedTransaction): Promise<string | Uint8Array>;
    };
    return {
      kind: "object",
      address: async () => null,
      prepareChain: async () => {},
      sign: async (tx) => ({
        kind: "raw",
        value: classifySigned(await s.signTransaction(tx)).value,
      }),
    };
  }
  if (isFn(o.sendTransaction)) {
    const s = signer as {
      sendTransaction(tx: PreparedTransaction): Promise<string>;
    };
    return {
      kind: "object",
      address: async () => null,
      prepareChain: async () => {},
      sign: async (tx) => ({
        kind: "hash",
        value: toHex(await s.sendTransaction(tx)),
      }),
    };
  }
  throw new TypeError(
    "unsupported signer: pass an EIP-1193 provider, a viem WalletClient, an ethers Signer, " +
      "a function (tx) => rawTx | txHash, or an object with signTransaction",
  );
}
