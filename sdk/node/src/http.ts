/**
 * HTTP transport for Arkivector SDK
 * Docs: docs/api/conventions.md §1, §4, §8, docs/api/errors.md
 */

import * as crypto from "node:crypto";
import { ApiError, TransportError, errorFromProblem } from "./errors.js";

/**
 * Convert camelCase string to snake_case.
 */
/**
 * Remove trailing "/" characters. A loop instead of /\/+$/, which backtracks polynomially on
 * long runs of slashes (CodeQL js/polynomial-redos).
 */
export function trimTrailingSlashes(s: string): string {
  let end = s.length;
  while (end > 0 && s.charCodeAt(end - 1) === 47) end--;
  return s.slice(0, end);
}

export function camelToSnake(str: string): string {
  return str.replace(/([a-z0-9])([A-Z])/g, "$1_$2").toLowerCase();
}

/**
 * Convert snake_case string to camelCase.
 */
export function snakeToCamel(str: string): string {
  return str.replace(/_([a-z0-9])/g, (_, letter) => letter.toUpperCase());
}

/**
 * Deeply convert object keys from camelCase to snake_case.
 * Leaves Uint8Array and ArrayBuffer untouched.
 */
export function toSnakeCaseKeys<T = unknown>(obj: unknown): T {
  if (obj === null || obj === undefined || typeof obj !== "object") {
    return obj as T;
  }
  if (obj instanceof Uint8Array || obj instanceof ArrayBuffer) {
    return obj as T;
  }
  if (Array.isArray(obj)) {
    return obj.map((item) => toSnakeCaseKeys(item)) as unknown as T;
  }
  const result: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(obj)) {
    result[camelToSnake(key)] = toSnakeCaseKeys(value);
  }
  return result as T;
}

/**
 * Deeply convert object keys from snake_case to camelCase.
 * Leaves Uint8Array and ArrayBuffer untouched.
 */
export function toCamelCaseKeys<T = unknown>(obj: unknown): T {
  if (obj === null || obj === undefined || typeof obj !== "object") {
    return obj as T;
  }
  if (obj instanceof Uint8Array || obj instanceof ArrayBuffer) {
    return obj as T;
  }
  if (Array.isArray(obj)) {
    return obj.map((item) => toCamelCaseKeys(item)) as unknown as T;
  }
  const result: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(obj)) {
    result[snakeToCamel(key)] = toCamelCaseKeys(value);
  }
  return result as T;
}

/**
 * Calculate exponential backoff with +/-20% jitter.
 * Base: 200 ms * 2^attempt
 */
export function calculateBackoff(attempt: number): number {
  const base = 200 * Math.pow(2, attempt);
  const jitter = 0.8 + Math.random() * 0.4; // [0.8, 1.2)
  return Math.round(base * jitter);
}

/**
 * Sleep helper that respects AbortSignal.
 */
export function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(
        new TransportError("Request aborted by caller", {
          code: "aborted",
          retryable: false,
        }),
      );
      return;
    }
    const timer = setTimeout(resolve, ms);
    if (signal) {
      const onAbort = () => {
        clearTimeout(timer);
        reject(
          new TransportError("Request aborted by caller", {
            code: "aborted",
            retryable: false,
          }),
        );
      };
      signal.addEventListener("abort", onAbort, { once: true });
    }
  });
}

/**
 * Determine whether an operation is eligible for retry per conventions.md §8.
 */
export function isOperationRetryable(
  method: string,
  path: string,
  idempotencyKey?: string,
  explicitIdempotent?: boolean,
): boolean {
  if (explicitIdempotent !== undefined) {
    return explicitIdempotent;
  }
  const upperMethod = method.toUpperCase();
  if (upperMethod === "GET" || upperMethod === "HEAD") {
    return true;
  }
  if (idempotencyKey && idempotencyKey.length > 0) {
    return true;
  }
  const normalizedPath = trimTrailingSlashes(path.split("?")[0]);
  if (
    normalizedPath.endsWith("/search") ||
    normalizedPath.endsWith("/search:batch") ||
    normalizedPath.endsWith("/scroll") ||
    normalizedPath.endsWith("/count") ||
    normalizedPath.endsWith("/entities:prepare")
  ) {
    return true;
  }
  return false;
}

/**
 * Configuration options for HttpTransport.
 */
export interface HttpTransportOptions {
  baseUrl: string;
  apiKey?: string;
  timeoutMs?: number;
  maxRetries?: number;
  fetchFn?: typeof globalThis.fetch;
  userAgent?: string;
}

/**
 * Per-request options.
 */
export interface HttpRequestOptions {
  method?: string;
  path: string;
  body?: unknown;
  headers?: Record<string, string>;
  timeoutMs?: number;
  maxRetries?: number;
  idempotencyKey?: string;
  isIdempotent?: boolean;
  signal?: AbortSignal;
}

/**
 * Robust HTTP transport client for Arkivector daemon.
 */
export class HttpTransport {
  readonly baseUrl: string;
  readonly apiKey?: string;
  readonly timeoutMs: number;
  readonly maxRetries: number;
  readonly fetchFn: typeof globalThis.fetch;
  readonly userAgent: string;

  constructor(options: HttpTransportOptions) {
    this.baseUrl = trimTrailingSlashes(options.baseUrl);
    this.apiKey = options.apiKey;
    this.timeoutMs = options.timeoutMs ?? 10000;
    this.maxRetries = options.maxRetries ?? 3;
    this.fetchFn = options.fetchFn ?? globalThis.fetch;
    this.userAgent = options.userAgent ?? "arkivector-node/1.0";
  }

  /**
   * Execute an HTTP request with timeout, retries, and RFC 9457 problem error mapping.
   */
  async request<T = unknown>(options: HttpRequestOptions): Promise<T> {
    const method = (options.method ?? "GET").toUpperCase();
    const url = `${this.baseUrl}${options.path.startsWith("/") ? "" : "/"}${options.path}`;
    const maxRetries = options.maxRetries ?? this.maxRetries;
    const isRetryableOp = isOperationRetryable(
      method,
      options.path,
      options.idempotencyKey,
      options.isIdempotent,
    );

    const headers: Record<string, string> = {
      "user-agent": this.userAgent,
      "x-request-id":
        options.headers?.["x-request-id"] ??
        options.headers?.["X-Request-Id"] ??
        crypto.randomUUID(),
      ...options.headers,
    };

    if (this.apiKey) {
      headers["authorization"] = `Bearer ${this.apiKey}`;
    }

    let bodyJson: string | undefined = undefined;
    if (options.body !== undefined) {
      headers["content-type"] =
        headers["content-type"] ??
        headers["Content-Type"] ??
        "application/json";
      bodyJson =
        typeof options.body === "string"
          ? options.body
          : JSON.stringify(options.body);
    }

    if (options.idempotencyKey) {
      headers["idempotency-key"] = options.idempotencyKey;
    }

    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      const controller = new AbortController();
      let timedOut = false;
      const timeoutMs = options.timeoutMs ?? this.timeoutMs;
      const timer = setTimeout(() => {
        timedOut = true;
        controller.abort();
      }, timeoutMs);

      let removeCallerListener: (() => void) | undefined;
      if (options.signal) {
        if (options.signal.aborted) {
          clearTimeout(timer);
          throw new TransportError("Request aborted by caller", {
            code: "aborted",
            retryable: false,
          });
        }
        const onCallerAbort = () => controller.abort();
        options.signal.addEventListener("abort", onCallerAbort, { once: true });
        removeCallerListener = () =>
          options.signal?.removeEventListener("abort", onCallerAbort);
      }

      try {
        const res = await this.fetchFn(url, {
          method,
          headers,
          body: bodyJson,
          signal: controller.signal,
        });
        clearTimeout(timer);
        removeCallerListener?.();

        if (res.ok) {
          if (res.status === 204) {
            return undefined as T;
          }
          const text = await res.text();
          if (!text || text.trim().length === 0) {
            return undefined as T;
          }
          return JSON.parse(text) as T;
        }

        // Non-2xx response: attempt RFC 9457 problem parsing
        const text = await res.text();
        let parsedProblem: Record<string, unknown> | null = null;
        if (text && text.trim().length > 0) {
          try {
            parsedProblem = JSON.parse(text) as Record<string, unknown>;
          } catch {
            // Non-JSON response
          }
        }

        let apiError: ApiError;
        if (parsedProblem && typeof parsedProblem === "object") {
          if (parsedProblem.status === undefined) {
            parsedProblem.status = res.status;
          }
          if (parsedProblem.request_id === undefined) {
            parsedProblem.request_id =
              res.headers.get("x-request-id") ?? headers["x-request-id"];
          }
          apiError = errorFromProblem(parsedProblem);
        } else {
          const isRetryableStatus =
            res.status === 429 ||
            res.status === 502 ||
            res.status === 503 ||
            res.status === 504;
          apiError = new ApiError(res.statusText || `HTTP ${res.status}`, {
            status: res.status,
            code: "http_error",
            retryable: isRetryableStatus,
            requestId:
              res.headers.get("x-request-id") ?? headers["x-request-id"],
            raw: text,
          });
        }

        // Retry check
        if (isRetryableOp && apiError.retryable && attempt < maxRetries) {
          let delayMs = calculateBackoff(attempt);
          const retryAfterHeader =
            res.headers.get("Retry-After") ?? res.headers.get("retry-after");
          if (retryAfterHeader) {
            const sec = parseFloat(retryAfterHeader);
            if (!isNaN(sec) && sec >= 0) {
              delayMs = Math.max(delayMs, sec * 1000);
            }
          }
          await sleep(delayMs, options.signal);
          continue;
        }

        throw apiError;
      } catch (err: unknown) {
        clearTimeout(timer);
        removeCallerListener?.();

        if (options.signal?.aborted) {
          throw new TransportError("Request aborted by caller", {
            code: "aborted",
            retryable: false,
          });
        }

        if (err instanceof ApiError) {
          throw err;
        }

        let transportError: TransportError;
        if (timedOut) {
          transportError = new TransportError(
            `Request timed out after ${timeoutMs}ms`,
            {
              code: "timeout",
              retryable: true,
            },
          );
        } else {
          transportError = new TransportError(
            err instanceof Error ? err.message : String(err),
            {
              code: "transport_error",
              retryable: true,
            },
          );
        }

        if (isRetryableOp && transportError.retryable && attempt < maxRetries) {
          const delayMs = calculateBackoff(attempt);
          await sleep(delayMs, options.signal);
          continue;
        }

        throw transportError;
      }
    }

    throw new TransportError("Max retries exceeded", {
      code: "max_retries_exceeded",
    });
  }

  async get<T = unknown>(
    path: string,
    options?: Omit<HttpRequestOptions, "path" | "method">,
  ): Promise<T> {
    return this.request<T>({ ...options, path, method: "GET" });
  }

  async post<T = unknown>(
    path: string,
    body?: unknown,
    options?: Omit<HttpRequestOptions, "path" | "method" | "body">,
  ): Promise<T> {
    return this.request<T>({ ...options, path, method: "POST", body });
  }

  async put<T = unknown>(
    path: string,
    body?: unknown,
    options?: Omit<HttpRequestOptions, "path" | "method" | "body">,
  ): Promise<T> {
    return this.request<T>({ ...options, path, method: "PUT", body });
  }

  async delete<T = unknown>(
    path: string,
    options?: Omit<HttpRequestOptions, "path" | "method">,
  ): Promise<T> {
    return this.request<T>({ ...options, path, method: "DELETE" });
  }
}
