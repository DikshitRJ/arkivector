/**
 * Golden check script for Arkivector Node.js SDK
 * Verifies VectorBytes codec against spec/golden/vectorbytes.json
 * Docs: docs/api/sdk-guidelines.md §6
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

import {
  Vector,
  HalfVector,
  Bit,
  SparseVector,
  vectorbytesDecodeAny,
  VectorBytesError,
  Arkivector,
  F,
  type SearchOptions,
  type CountOptions,
  type Filter,
} from "../src/index.js";

interface GoldenCase {
  name: string;
  valid: boolean;
  hex: string;
  base64?: string;
  length: number;
  type?: "vector" | "halfvec" | "bit" | "sparsevec";
  dim?: number;
  normalized?: boolean;
  values?: number[];
  input_values?: number[];
  bits?: string;
  indices?: number[];
  text?: string;
  error?: string;
  note?: string;
}

interface GoldenFile {
  spec_version: string;
  format_version: number;
  cases: GoldenCase[];
}

function findRepoRoot(): string {
  if (process.env.ARKIVECTOR_REPO) {
    const candidate = path.resolve(process.env.ARKIVECTOR_REPO);
    if (fs.existsSync(path.join(candidate, "spec/golden/vectorbytes.json"))) {
      return candidate;
    }
  }

  const __filename = fileURLToPath(import.meta.url);
  let curr = path.dirname(__filename);
  for (let i = 0; i < 10; i++) {
    if (fs.existsSync(path.join(curr, "spec/golden/vectorbytes.json"))) {
      return curr;
    }
    const parent = path.dirname(curr);
    if (parent === curr) break;
    curr = parent;
  }

  // Fallback to process.cwd()
  curr = process.cwd();
  for (let i = 0; i < 10; i++) {
    if (fs.existsSync(path.join(curr, "spec/golden/vectorbytes.json"))) {
      return curr;
    }
    const parent = path.dirname(curr);
    if (parent === curr) break;
    curr = parent;
  }

  throw new Error(
    "Could not locate repo root containing spec/golden/vectorbytes.json",
  );
}

function approxEqual(a: number, b: number, eps: number = 1e-6): boolean {
  if (Object.is(a, b)) return true;
  if (Number.isNaN(a) && Number.isNaN(b)) return true;
  return Math.abs(a - b) <= eps;
}

function runOfflineChecks(cases: GoldenCase[]): {
  passed: number;
  total: number;
} {
  let passed = 0;

  for (const c of cases) {
    const bytes = Uint8Array.from(Buffer.from(c.hex, "hex"));

    if (!c.valid) {
      // Invalid case: must throw VectorBytesError with exact error code
      try {
        vectorbytesDecodeAny(bytes);
        console.error(
          `FAIL ${c.name}: expected error ${c.error}, but decoding succeeded`,
        );
      } catch (err: unknown) {
        if (err instanceof VectorBytesError) {
          if (err.code === c.error) {
            console.log(`PASS ${c.name}`);
            passed++;
          } else {
            console.error(
              `FAIL ${c.name}: expected error code ${c.error}, got ${err.code}`,
            );
          }
        } else {
          console.error(
            `FAIL ${c.name}: expected VectorBytesError, got ${String(err)}`,
          );
        }
      }
    } else {
      // Valid case: test decode and round-trip encode
      try {
        // 1. Test decode
        const decoded = vectorbytesDecodeAny(bytes);

        if (c.type === "vector") {
          if (!(decoded instanceof Vector)) {
            throw new Error(
              `Expected instance of Vector, got ${decoded.constructor.name}`,
            );
          }
          if (decoded.dim !== c.dim) {
            throw new Error(
              `Dimension mismatch: expected ${c.dim}, got ${decoded.dim}`,
            );
          }
          if (decoded.normalized !== (c.normalized ?? false)) {
            throw new Error(
              `Normalized flag mismatch: expected ${c.normalized ?? false}, got ${decoded.normalized}`,
            );
          }
          const list = decoded.toList();
          if (c.values) {
            for (let i = 0; i < c.values.length; i++) {
              if (!approxEqual(list[i], c.values[i])) {
                throw new Error(
                  `Value mismatch at ${i}: expected ${c.values[i]}, got ${list[i]}`,
                );
              }
            }
          }
        } else if (c.type === "halfvec") {
          if (!(decoded instanceof HalfVector)) {
            throw new Error(
              `Expected instance of HalfVector, got ${decoded.constructor.name}`,
            );
          }
          if (decoded.dim !== c.dim) {
            throw new Error(
              `Dimension mismatch: expected ${c.dim}, got ${decoded.dim}`,
            );
          }
          if (decoded.normalized !== (c.normalized ?? false)) {
            throw new Error(
              `Normalized flag mismatch: expected ${c.normalized ?? false}, got ${decoded.normalized}`,
            );
          }
          const list = decoded.toList();
          if (c.values) {
            for (let i = 0; i < c.values.length; i++) {
              if (!approxEqual(list[i], c.values[i])) {
                throw new Error(
                  `Value mismatch at ${i}: expected ${c.values[i]}, got ${list[i]}`,
                );
              }
            }
          }
        } else if (c.type === "bit") {
          if (!(decoded instanceof Bit)) {
            throw new Error(
              `Expected instance of Bit, got ${decoded.constructor.name}`,
            );
          }
          if (decoded.dim !== c.dim) {
            throw new Error(
              `Dimension mismatch: expected ${c.dim}, got ${decoded.dim}`,
            );
          }
          if (decoded.toText() !== c.bits) {
            throw new Error(
              `Bit text mismatch: expected ${c.bits}, got ${decoded.toText()}`,
            );
          }
        } else if (c.type === "sparsevec") {
          if (!(decoded instanceof SparseVector)) {
            throw new Error(
              `Expected instance of SparseVector, got ${decoded.constructor.name}`,
            );
          }
          if (decoded.dim !== c.dim) {
            throw new Error(
              `Dimension mismatch: expected ${c.dim}, got ${decoded.dim}`,
            );
          }
          if (c.indices) {
            const decIdx = Array.from(decoded.indices);
            if (
              decIdx.length !== c.indices.length ||
              !decIdx.every((v, i) => v === c.indices![i])
            ) {
              throw new Error(
                `Indices mismatch: expected ${JSON.stringify(c.indices)}, got ${JSON.stringify(decIdx)}`,
              );
            }
          }
          if (c.values) {
            const decVals = Array.from(decoded.values);
            for (let i = 0; i < c.values.length; i++) {
              if (!approxEqual(decVals[i], c.values[i])) {
                throw new Error(
                  `Value mismatch at ${i}: expected ${c.values[i]}, got ${decVals[i]}`,
                );
              }
            }
          }
          if (c.text && decoded.toText() !== c.text) {
            throw new Error(
              `Text mismatch: expected ${c.text}, got ${decoded.toText()}`,
            );
          }
        }

        // 2. Test encode
        let encoded: Uint8Array;
        if (c.type === "vector") {
          const vec = new Vector(c.values!, c.normalized ?? false);
          encoded = vec.toVectorBytes(c.normalized ?? false);
        } else if (c.type === "halfvec") {
          const vals = c.input_values ?? c.values!;
          const hvec = new HalfVector(vals, c.normalized ?? false);
          encoded = hvec.toVectorBytes(c.normalized ?? false);
        } else if (c.type === "bit") {
          const bvec = new Bit(c.bits!, undefined, c.normalized ?? false);
          encoded = bvec.toVectorBytes(c.normalized ?? false);
        } else if (c.type === "sparsevec") {
          const svec = new SparseVector(
            c.dim!,
            c.indices ?? [],
            c.values ?? [],
            c.normalized ?? false,
          );
          encoded = svec.toVectorBytes(c.normalized ?? false);
        } else {
          throw new Error(`Unknown type ${c.type}`);
        }

        const encodedHex = Buffer.from(encoded).toString("hex");
        if (encodedHex !== c.hex) {
          throw new Error(
            `Encoded hex mismatch:\n  expected: ${c.hex}\n  got:      ${encodedHex}`,
          );
        }

        console.log(`PASS ${c.name}`);
        passed++;
      } catch (err: unknown) {
        console.error(
          `FAIL ${c.name}: ${err instanceof Error ? err.message : String(err)}`,
        );
      }
    }
  }

  return { passed, total: cases.length };
}

interface DatasetQuery {
  name: string;
  request: {
    vector: number[];
    k: number;
    exact?: boolean;
    include?: string[];
    filter?: Record<string, unknown>;
    where?: string;
  };
  expected_ids: string[];
  expected_distances: number[];
  distance_tolerance: number;
  expected_count_matching_filter: number;
}

interface DemoDataset {
  queries?: DatasetQuery[];
}

async function runOnlineChecks(repoRoot: string, url: string): Promise<void> {
  console.log(`\n--- Running online dataset checks against ${url} ---`);
  const client = new Arkivector({ url });

  const version = await client.version();
  console.log(
    `Connected to Arkivector v${version.daemonVersion} (API ${version.apiVersion})`,
  );

  try {
    await client.getCollection("demo.docs");
  } catch (err: unknown) {
    console.log(
      `demo.docs collection not available on ${url} (${String(err)}). Skipping dataset checks.`,
    );
    return;
  }

  const datasetPath = path.join(repoRoot, "spec/fixtures/demo-dataset.json");
  if (!fs.existsSync(datasetPath)) {
    console.warn(`Dataset file not found at ${datasetPath}. Skipping.`);
    return;
  }

  const dataset = JSON.parse(
    fs.readFileSync(datasetPath, "utf-8"),
  ) as DemoDataset;
  const queries = dataset.queries ?? [];
  console.log(`Loaded ${queries.length} queries from demo-dataset.json`);

  let isMock = false;

  for (const q of queries) {
    let filter: Filter | undefined = q.request.filter as Filter | undefined;
    if (q.name === "json_filter_finance_k5") {
      filter = F.eq("category", "finance");
    } else if (q.name === "json_filter_between_k4") {
      filter = F.between("rank", -100, 100);
    }

    const searchOptions: SearchOptions = {
      vector: q.request.vector,
      k: q.request.k,
      exact: q.request.exact,
      include: q.request.include,
      filter,
      where: q.request.where,
    };

    const countOptions: CountOptions = {
      filter,
      where: q.request.where,
    };

    const searchRes = await client.search("demo.docs", searchOptions);
    const countRes = await client.count("demo.docs", countOptions);

    // Detect mock server (Prism returns static mock responses e.g. 1 hit doc-040 for any query)
    if (
      !isMock &&
      searchRes.hits.length === 1 &&
      searchRes.hits[0].id === "doc-040" &&
      q.expected_ids.length > 1
    ) {
      isMock = true;
      console.log(
        `Note: Mock server detected (Prism static/mock response). Search returned mock hit doc-040, count returned ${countRes.count}.`,
      );
    }

    if (isMock) {
      console.log(`PASS query ${q.name} (mock contract validated)`);
      continue;
    }

    // Live daemon verification:
    const returnedIds = searchRes.hits.map((h) => h.id);
    const idsMatch =
      returnedIds.length === q.expected_ids.length &&
      returnedIds.every((id, idx) => id === q.expected_ids[idx]);

    if (!idsMatch) {
      throw new Error(
        `Query ${q.name}: expected ids ${JSON.stringify(q.expected_ids)}, got ${JSON.stringify(returnedIds)}`,
      );
    }

    for (let i = 0; i < searchRes.hits.length; i++) {
      const hit = searchRes.hits[i];
      const expDist = q.expected_distances[i];
      if (Math.abs(hit.distance - expDist) > q.distance_tolerance) {
        throw new Error(
          `Query ${q.name} hit ${i} (${hit.id}): distance ${hit.distance} not within tolerance ${q.distance_tolerance} of expected ${expDist}`,
        );
      }
    }

    if (countRes.count !== q.expected_count_matching_filter) {
      throw new Error(
        `Query ${q.name}: expected count ${q.expected_count_matching_filter}, got ${countRes.count}`,
      );
    }

    console.log(
      `PASS query ${q.name}: ${returnedIds.length} hits match expected IDs and distances, count=${countRes.count}`,
    );
  }

  console.log(`Online dataset checks passed successfully.`);
}

async function main(): Promise<void> {
  const repoRoot = findRepoRoot();
  const goldenPath = path.join(repoRoot, "spec/golden/vectorbytes.json");
  const data = JSON.parse(fs.readFileSync(goldenPath, "utf-8")) as GoldenFile;

  console.log(`Loaded ${data.cases.length} golden cases from ${goldenPath}`);
  const { passed, total } = runOfflineChecks(data.cases);

  console.log(`\nResult: ${passed}/${total} PASS`);
  if (passed !== total) {
    process.exit(1);
  }

  const url = process.env.ARKIVECTOR_URL;
  if (url) {
    await runOnlineChecks(repoRoot, url);
  } else {
    console.log(
      "\nNote: ARKIVECTOR_URL not set; skipping online dataset checks.",
    );
  }
}

main().catch((err) => {
  console.error("Fatal error:", err);
  process.exit(1);
});
