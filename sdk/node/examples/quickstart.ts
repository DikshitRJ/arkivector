/**
 * Quickstart example for @arkivector/client
 * Demonstrates client connection, version/capabilities, ops, collections,
 * error handling, and the retry engine.
 */

import * as http from "node:http";
import {
  Arkivector,
  HttpTransport,
  ValidationError,
  NotImplementedByDaemonError,
  HalfVector,
  F,
  T,
  Ttl,
  EntityInput,
  buildEntityAttributes,
  toArkivSdkAttributes,
} from "../src/index.js";

async function main(): Promise<void> {
  const url = process.env.ARKIVECTOR_URL ?? "http://127.0.0.1:4010";
  console.log(`Connecting to Arkivector at ${url}...`);

  const client = new Arkivector({ url });

  // 1. Version and capabilities
  const version = await client.version();
  console.log(
    `Daemon version: ${version.daemonVersion}, API: ${version.apiVersion}, pgvector: ${version.pgvectorVersion}`,
  );
  console.log(
    `Kernel tier: ${version.kernelTier}, Chain ID: ${version.chainId}`,
  );
  console.log(
    `Capabilities (${version.capabilities.length}): ${version.capabilities.join(", ")}`,
  );

  // 2. Liveness and readiness
  const isHealthy = await client.health();
  console.log(`Health check: ${isHealthy ? "OK" : "FAILED"}`);

  const ready = await client.ready();
  console.log(
    `Readiness: ready=${ready.ready}, collections=${JSON.stringify(ready.collections)}`,
  );

  // 3. Sync status
  if (await client.hasCapability("ops.sync")) {
    const sync = await client.syncStatus();
    console.log(`Sync status for ${sync.length} collection(s):`);
    for (const s of sync) {
      console.log(
        `  - ${s.collection}: state=${s.state}, asOfBlock=${s.asOfBlock}`,
      );
    }
  }

  // 4. Collections
  const collections = await client.listCollections();
  console.log(`Followed collections (${collections.length}):`);
  for (const c of collections) {
    console.log(
      `  - ${c.name} (${c.schemaTag}): ${c.column.type}(${c.column.dimensions}) index=${c.index.method}(${c.index.opclass})`,
    );
  }

  // 5. Search demonstration on demo.docs
  console.log("\n--- Demonstrating search on demo.docs ---");
  try {
    const queryVector = new HalfVector([
      0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8,
    ]);
    const searchRes = await client.search("demo.docs", {
      vector: queryVector,
      k: 5,
      filter: F.eq("category", "finance"),
    });
    console.log(
      `Search returned ${searchRes.hits.length} hit(s) at asOfBlock=${searchRes.asOfBlock}, syncState=${searchRes.syncState}:`,
    );
    for (const hit of searchRes.hits) {
      console.log(
        `  - key=${hit.key.slice(0, 10)}... id=${hit.id} dist=${hit.distance} score=${hit.score} attrs=${JSON.stringify(hit.plainAttributes())}`,
      );
    }
  } catch (err: unknown) {
    console.log(`Search note/error: ${String(err)}`);
  }

  // 6. Error handling demonstration
  console.log("\n--- Demonstrating error handling ---");

  // Local checkCapability error
  try {
    await client.checkCapability("nonexistent.experimental.feature");
  } catch (err: unknown) {
    if (err instanceof NotImplementedByDaemonError) {
      console.log(
        `Caught expected local capability error: [${err.code}] ${err.message}`,
      );
    } else {
      throw err;
    }
  }

  // Remote validation error from daemon/Prism
  try {
    // Send invalid collection create to trigger 422
    await client.createCollection({
      name: "",
      column: { type: "vector", dimensions: 0 },
      index: { method: "none", opclass: "" },
    });
  } catch (err: unknown) {
    if (err instanceof ValidationError) {
      console.log(
        `Caught expected validation error: [status=${err.status}, code=${err.code}] ${err.message}`,
      );
    } else {
      console.log(`Caught error: ${String(err)}`);
    }
  }

  // 7. Retry engine demonstration
  console.log("\n--- Demonstrating retry mechanism ---");
  let serverAttempts = 0;
  const retryServer = http.createServer((_req, res) => {
    serverAttempts++;
    if (serverAttempts < 3) {
      res.writeHead(503, {
        "Content-Type": "application/problem+json",
        "Retry-After": "0.1",
      });
      res.end(
        JSON.stringify({
          type: "urn:arkivector:error:not_ready",
          title: "Not Ready",
          status: 503,
          code: "not_ready",
          retryable: true,
        }),
      );
    } else {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ status: "ok", attempts: serverAttempts }));
    }
  });

  await new Promise<void>((resolve) =>
    retryServer.listen(0, "127.0.0.1", resolve),
  );
  const addr = retryServer.address() as { port: number };
  const retryUrl = `http://127.0.0.1:${addr.port}`;

  const retryTransport = new HttpTransport({
    baseUrl: retryUrl,
    maxRetries: 3,
  });

  const startTime = Date.now();
  const retryResult = await retryTransport.get<{
    status: string;
    attempts: number;
  }>("/retry-test");
  const elapsed = Date.now() - startTime;

  console.log(
    `Retry succeeded: status=${retryResult.status}, server received ${retryResult.attempts} requests after ${elapsed}ms`,
  );

  await new Promise<void>((resolve, reject) =>
    retryServer.close((err) => (err ? reject(err) : resolve())),
  );

  // 8. Write operations (--write)
  const isWrite = process.argv.includes("--write");
  if (isWrite) {
    console.log("\n--- Demonstrating write operations (--write) ---");
    const writeClient = new Arkivector({ url, sessionConsistency: true });

    const writeEntities: EntityInput[] = [
      {
        id: "quickstart-doc-1",
        vector: new HalfVector([0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8]),
        document: "First quickstart document about blockchain technology.",
        attributes: {
          category: "tech",
          score: T.dec("4.8"),
        },
        ttl: Ttl.days(30),
      },
      {
        id: "quickstart-doc-2",
        vector: new HalfVector([0.2, -0.3, 0.4, 0.5, -0.6, 0.7, 0.8, -0.9]),
        document: "Second quickstart document about decentralized finance.",
        attributes: {
          category: "finance",
          score: T.dec("4.2"),
        },
        ttl: Ttl.days(30),
      },
      {
        id: "quickstart-doc-3",
        vector: new HalfVector([0.3, -0.4, 0.5, 0.6, -0.7, 0.8, 0.9, -1.0]),
        document: "Third quickstart document about vector embeddings.",
        attributes: {
          category: "ai",
          score: T.dec("4.9"),
        },
        ttl: Ttl.days(30),
      },
    ];

    console.log(`Upserting ${writeEntities.length} entities to demo.docs...`);
    const upsertRes = await writeClient.upsert("demo.docs", writeEntities);
    console.log(
      `Upsert succeeded: ${upsertRes.results.length} result(s), txHashes=${upsertRes.txHashes.join(",")}, blockNumber=${upsertRes.blockNumber}`,
    );
    console.log(`writeClient.lastWriteBlock: ${writeClient.lastWriteBlock}`);
    if (upsertRes.results.length === 0) {
      throw new Error("Upsert returned 0 results");
    }

    console.log(
      `Searching with session consistency (lastWriteBlock=${writeClient.lastWriteBlock})...`,
    );
    const searchAfterWrite = await writeClient.search("demo.docs", {
      vector: new HalfVector([0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, -0.8]),
      k: 5,
    });
    console.log(
      `Search returned ${searchAfterWrite.hits.length} hit(s) at asOfBlock=${searchAfterWrite.asOfBlock}`,
    );

    // Session consistency: this read waits until the daemon has synced our write block.
    const written = await writeClient.get("demo.docs", "quickstart-doc-1");
    console.log(`Read back ${written.id}`);
    console.log("Deleting written entities...");
    const delRes = await writeClient.delete("demo.docs", {
      refs: ["quickstart-doc-1", "quickstart-doc-2", "quickstart-doc-3"],
    });
    console.log(
      `Delete succeeded: affected=${delRes.affected}, txHashes=${delRes.txHashes.join(",")}, blockNumber=${delRes.blockNumber}`,
    );

    console.log("Demonstrating prepare for client-side signing...");
    const prepRes = await writeClient.prepare("demo.docs", [{ ...writeEntities[0], id: "quickstart-prepare-1" }], {
      from: "0x1111111111111111111111111111111111111111",
    });
    console.log(
      `Prepare succeeded: chainId=${prepRes.chainId}, ${prepRes.transactions.length} tx(s), ${prepRes.results.length} predicted key(s)`,
    );

    console.log("Verifying direct-write attribute builders...");
    const colDef = await writeClient.getCollection("demo.docs");
    const directAttrs = buildEntityAttributes(colDef, writeEntities[0]);
    console.log(
      `buildEntityAttributes produced ${directAttrs.length} attributes:`,
    );
    for (const a of directAttrs) {
      console.log(
        `  - ${a.name} (typeId=0x${a.typeId.toString(16)}, ${a.valueBytes.length} bytes)`,
      );
    }
    if (
      directAttrs[0].name !== "arkiv.schema" ||
      directAttrs[0].typeId !== 0x88
    ) {
      throw new Error("Expected directAttrs[0] to be arkiv.schema (0x88)");
    }
    if (directAttrs[1].name !== "av.vec" || directAttrs[1].typeId !== 0x07) {
      throw new Error("Expected directAttrs[1] to be av.vec (0x07)");
    }
    if (directAttrs[2].name !== "av.id" || directAttrs[2].typeId !== 0x88) {
      throw new Error("Expected directAttrs[2] to be av.id (0x88)");
    }
    if (directAttrs[3].name !== "av.doc" || directAttrs[3].typeId !== 0x07) {
      throw new Error("Expected directAttrs[3] to be av.doc (0x07)");
    }

    const sdkAttrs = toArkivSdkAttributes(colDef, writeEntities[0]);
    console.log(
      `toArkivSdkAttributes produced keys: ${Object.keys(sdkAttrs).join(", ")}`,
    );
    if (!sdkAttrs["arkiv.schema"] || !sdkAttrs["av.vec"]) {
      throw new Error("Expected sdkAttrs to contain arkiv.schema and av.vec");
    }

    console.log("Write operations verified successfully.");
  }

  console.log("\nQuickstart finished successfully.");
}

main().catch((err) => {
  console.error("Quickstart error:", err);
  process.exit(1);
});
