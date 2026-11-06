/**
 * Arkivector error hierarchy
 * Docs: docs/api/errors.md
 */

/**
 * Local codec error raised by the SDK's own VectorBytes encoder/decoder.
 * Code is one of the VB_* error codes.
 */
export class VectorBytesError extends Error {
  readonly code: string;

  constructor(code: string, message?: string) {
    super(message ?? `VectorBytes error: ${code}`);
    this.name = "VectorBytesError";
    this.code = code;
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Common options for constructing API errors.
 */
export interface ApiErrorOptions {
  code: string;
  status?: number;
  title?: string;
  detail?: string;
  retryable?: boolean;
  requestId?: string;
  field?: string;
  position?: number;
  vbError?: string;
  asOfBlock?: number;
  revertError?: string;
  raw?: unknown;
}

/**
 * Base class for all Arkivector API / HTTP errors.
 */
export class ApiError extends Error {
  readonly code: string;
  readonly status?: number;
  readonly title?: string;
  readonly detail?: string;
  readonly retryable: boolean;
  readonly requestId?: string;
  readonly field?: string;
  readonly position?: number;
  readonly vbError?: string;
  readonly asOfBlock?: number;
  readonly revertError?: string;
  readonly raw?: unknown;

  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    let msg: string;
    let opts: Partial<ApiErrorOptions>;

    if (typeof messageOrOptions === "string") {
      msg = messageOrOptions;
      opts = options ?? {};
    } else {
      opts = messageOrOptions;
      msg =
        opts.detail ??
        opts.title ??
        `Arkivector API error: ${opts.code ?? "unknown"}`;
    }

    super(msg);
    this.name = "ApiError";
    this.code = opts.code ?? "api_error";
    this.status = opts.status;
    this.title = opts.title;
    this.detail = opts.detail;
    this.retryable = opts.retryable ?? false;
    this.requestId = opts.requestId;
    this.field = opts.field;
    this.position = opts.position;
    this.vbError = opts.vbError;
    this.asOfBlock = opts.asOfBlock;
    this.revertError = opts.revertError;
    this.raw = opts.raw;
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Network / transport error when no HTTP response is received.
 */
export class TransportError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "TransportError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Authentication / authorization error (unauthorized, forbidden, write_disabled).
 */
export class AuthError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "AuthError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Resource not found error (*_not_found).
 */
export class NotFoundError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "NotFoundError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Conflict error (collection_exists, not_owner, tx_reverted, idempotency_conflict).
 */
export class ConflictError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "ConflictError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Validation error (bad_request, invalid_*, dimension_mismatch, payload_too_large, unsupported_media_type).
 */
export class ValidationError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "ValidationError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Service unavailable or temporary failure (rate_limited, arkiv_rpc_error, not_ready, timeout, consistency_timeout).
 */
export class UnavailableError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "UnavailableError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Feature not implemented by this daemon build (not_implemented).
 */
export class NotImplementedByDaemonError extends ApiError {
  constructor(
    messageOrOptions: string | ApiErrorOptions,
    options?: Partial<ApiErrorOptions>,
  ) {
    super(messageOrOptions, options);
    this.name = "NotImplementedByDaemonError";
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

/**
 * Map RFC 9457 problem JSON to the corresponding ApiError subclass.
 */
export function errorFromProblem(problem: Record<string, unknown>): ApiError {
  const code = typeof problem.code === "string" ? problem.code : "api_error";
  const status =
    typeof problem.status === "number" ? problem.status : undefined;
  const title = typeof problem.title === "string" ? problem.title : undefined;
  const detail =
    typeof problem.detail === "string" ? problem.detail : undefined;
  const retryable =
    typeof problem.retryable === "boolean" ? problem.retryable : false;
  const requestId =
    typeof problem.request_id === "string" ? problem.request_id : undefined;
  const field = typeof problem.field === "string" ? problem.field : undefined;
  const position =
    typeof problem.position === "number" ? problem.position : undefined;
  const vbError =
    typeof problem.vb_error === "string" ? problem.vb_error : undefined;
  const asOfBlock =
    typeof problem.as_of_block === "number" ? problem.as_of_block : undefined;
  const revertError =
    typeof problem.revert_error === "string" ? problem.revert_error : undefined;

  const opts: ApiErrorOptions = {
    code,
    status,
    title,
    detail,
    retryable,
    requestId,
    field,
    position,
    vbError,
    asOfBlock,
    revertError,
    raw: problem,
  };

  const message = detail ?? title ?? `API error: ${code}`;

  switch (code) {
    case "unauthorized":
    case "forbidden":
    case "write_disabled":
      return new AuthError(message, opts);

    case "collection_not_found":
    case "entity_not_found":
    case "route_not_found":
      return new NotFoundError(message, opts);

    case "collection_exists":
    case "not_owner":
    case "tx_reverted":
    case "idempotency_conflict":
      return new ConflictError(message, opts);

    case "bad_request":
    case "invalid_argument":
    case "invalid_vector":
    case "dimension_mismatch":
    case "invalid_filter":
    case "payload_too_large":
    case "unsupported_media_type":
      return new ValidationError(message, opts);

    case "rate_limited":
    case "arkiv_rpc_error":
    case "not_ready":
    case "timeout":
    case "consistency_timeout":
      return new UnavailableError(message, opts);

    case "not_implemented":
      return new NotImplementedByDaemonError(message, opts);

    default:
      return new ApiError(message, opts);
  }
}
