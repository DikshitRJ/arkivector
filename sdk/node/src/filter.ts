/**
 * Arkivector Filter builder (F)
 * Docs: docs/api/filters.md §2, §3
 */

import { TypedValue, T, AttrType } from "./models.js";

/**
 * Filter value types accepted by comparison predicates.
 */
export type FilterValue =
  | TypedValue
  | { type: AttrType | string; value: unknown }
  | string
  | number
  | boolean
  | bigint;

/**
 * Filter AST node shapes matching docs/api/filters.md §2.
 */
export type Filter =
  | { and: Filter[] }
  | { or: Filter[] }
  | { not: Filter }
  | { eq: Record<string, FilterValue> }
  | { ne: Record<string, FilterValue> }
  | { lt: Record<string, FilterValue> }
  | { lte: Record<string, FilterValue> }
  | { gt: Record<string, FilterValue> }
  | { gte: Record<string, FilterValue> }
  | { between: Record<string, [FilterValue, FilterValue]> }
  | { startswith: Record<string, string> }
  | { exists: string }
  | { typeof: Record<string, AttrType | string> };

/**
 * Coerce a raw scalar value into an explicit TypedValue per docs/api/filters.md §2:
 * - TypedValue is preserved
 * - string -> T.str
 * - boolean -> T.bool
 * - integer number -> T.i32
 * - bigint -> T.u64
 * - non-integer float -> throws TypeError instructing caller to use explicit T.dec(...)
 */
export function toFilterValue(val: unknown): FilterValue {
  if (val instanceof TypedValue) {
    return val;
  }
  if (
    val !== null &&
    typeof val === "object" &&
    "type" in val &&
    "value" in val
  ) {
    const obj = val as { type: AttrType; value: unknown };
    return new TypedValue(obj.type, obj.value);
  }
  if (typeof val === "string") {
    return T.str(val);
  }
  if (typeof val === "boolean") {
    return T.bool(val);
  }
  if (typeof val === "number") {
    if (Number.isInteger(val)) {
      return T.i32(val);
    }
    throw new TypeError(
      `Floating point number ${val} is ambiguous in filters; use T.dec("${val}") instead.`,
    );
  }
  if (typeof val === "bigint") {
    return T.u64(val);
  }
  throw new TypeError(
    `Unsupported filter value type: ${typeof val} (${String(val)})`,
  );
}

function normalizeChildren(children: (Filter | Filter[])[]): Filter[] {
  if (children.length === 1 && Array.isArray(children[0])) {
    return children[0];
  }
  return children as Filter[];
}

/**
 * Filter builder F.
 * Produces exact JSON AST nodes per docs/api/filters.md §2.
 */
export const F = {
  /**
   * Logical AND of one or more filters.
   * Accepts varargs: F.and(a, b) or an array: F.and([a, b]).
   */
  and(...children: (Filter | Filter[])[]): { and: Filter[] } {
    const list = normalizeChildren(children);
    if (list.length === 0) {
      throw new TypeError("F.and requires at least 1 child filter");
    }
    return { and: list };
  },

  /**
   * Logical OR of one or more filters.
   * Accepts varargs: F.or(a, b) or an array: F.or([a, b]).
   */
  or(...children: (Filter | Filter[])[]): { or: Filter[] } {
    const list = normalizeChildren(children);
    if (list.length === 0) {
      throw new TypeError("F.or requires at least 1 child filter");
    }
    return { or: list };
  },

  /**
   * Logical NOT of a single filter.
   */
  not(child: Filter): { not: Filter } {
    if (!child || typeof child !== "object") {
      throw new TypeError("F.not requires a valid child filter");
    }
    return { not: child };
  },

  /**
   * Field equality: {"eq": {"<field>": Value}}
   */
  eq(field: string, value: unknown): { eq: Record<string, FilterValue> } {
    return { eq: { [field]: toFilterValue(value) } };
  },

  /**
   * Field inequality: {"ne": {"<field>": Value}}
   */
  ne(field: string, value: unknown): { ne: Record<string, FilterValue> } {
    return { ne: { [field]: toFilterValue(value) } };
  },

  /**
   * Less than: {"lt": {"<field>": Value}}
   */
  lt(field: string, value: unknown): { lt: Record<string, FilterValue> } {
    return { lt: { [field]: toFilterValue(value) } };
  },

  /**
   * Less than or equal: {"lte": {"<field>": Value}}
   */
  lte(field: string, value: unknown): { lte: Record<string, FilterValue> } {
    return { lte: { [field]: toFilterValue(value) } };
  },

  /**
   * Greater than: {"gt": {"<field>": Value}}
   */
  gt(field: string, value: unknown): { gt: Record<string, FilterValue> } {
    return { gt: { [field]: toFilterValue(value) } };
  },

  /**
   * Greater than or equal: {"gte": {"<field>": Value}}
   */
  gte(field: string, value: unknown): { gte: Record<string, FilterValue> } {
    return { gte: { [field]: toFilterValue(value) } };
  },

  /**
   * Range between low and high (inclusive): {"between": {"<field>": [low, high]}}
   */
  between(
    field: string,
    low: unknown,
    high: unknown,
  ): { between: Record<string, [FilterValue, FilterValue]> } {
    return {
      between: {
        [field]: [toFilterValue(low), toFilterValue(high)],
      },
    };
  },

  /**
   * Prefix match for string fields: {"startswith": {"<field>": "prefix"}}
   */
  startswith(
    field: string,
    prefix: string,
  ): { startswith: Record<string, string> } {
    if (typeof prefix !== "string") {
      throw new TypeError(
        `F.startswith prefix must be a string, got ${typeof prefix}`,
      );
    }
    return { startswith: { [field]: prefix } };
  },

  /**
   * Field existence check: {"exists": "<field>"}
   */
  exists(field: string): { exists: string } {
    if (typeof field !== "string") {
      throw new TypeError(
        `F.exists field must be a string, got ${typeof field}`,
      );
    }
    return { exists: field };
  },

  /**
   * Field type check: {"typeof": {"<field>": "<AttrType>"}}
   */
  typeof(
    field: string,
    attrType: AttrType | string,
  ): { typeof: Record<string, AttrType | string> } {
    if (typeof field !== "string") {
      throw new TypeError(
        `F.typeof field must be a string, got ${typeof field}`,
      );
    }
    return { typeof: { [field]: attrType } };
  },
};
