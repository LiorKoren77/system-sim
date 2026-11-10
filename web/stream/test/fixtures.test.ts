// The committed fixtures are datasets the precompute wrote with the real simsys (npm run fixtures;
// CI's web job checks they are still what simsys writes). They pin the format these tests read.
import { readFileSync } from "node:fs";
import { gunzipSync } from "node:zlib";
import { describe, expect, it } from "vitest";
import { MemorySource } from "../src/memory-source";
import { bucketBalance, DatasetStore } from "../src/store";

const names = ["tutorial-hello", "tutorial-chain", "labs-kv-store", "book-url-shortener"];

async function fixture(name: string): Promise<DatasetStore> {
  const gz = readFileSync(new URL(`./fixtures/${name}.ndjson.gz`, import.meta.url));
  const lines = gunzipSync(gz)
    .toString("utf8")
    .split("\n")
    .filter((line) => line !== "");
  const store = new DatasetStore();
  await store.consume(new MemorySource(lines));
  return store;
}

// The rows of nodes with a queue: everything but a client, whose row is all null.
function queues(store: DatasetStore): number[] {
  return (store.graph?.nodes ?? []).flatMap((node, row) => (node.type === "client" ? [] : [row]));
}

describe("the committed fixtures", () => {
  it.each(names)("%s reads to the end, its summary included", async (name) => {
    const store = await fixture(name);
    expect(store.status).toBe("ready");
    expect(store.dataset?.id).toBe(name.replace("-", "/"));
  });

  it.each(names)("%s keeps the conservation law in every bucket of every queue", async (name) => {
    // demand · dt = (drain + dropped) · dt + Δbacklog, to the six significant digits a frame keeps:
    // the tolerance is relative to the bucket's inflow, with a floor for an idle bucket.
    const store = await fixture(name);
    for (const row of queues(store)) {
      for (let k = 1; k < store.frameCount; ++k) {
        const { inflow, outflow } = bucketBalance(store, row, k);
        if (Number.isNaN(inflow)) continue; // a node type whose demand is not a queue's
        expect(Math.abs(outflow - inflow)).toBeLessThanOrEqual(1e-4 * Math.abs(inflow) + 1e-3);
      }
    }
  });

  it("url-shortener's day repeats, keeps its second day, and loops without breaking the law", async () => {
    const store = await fixture("book-url-shortener");
    expect(store.dataset?.kind).toBe("day");
    expect(store.dataset?.loops).toBe(true);
    expect(store.frameCount).toBe(720);
    expect(store.times()[0]).toBe(86400);
    // The loop's seam: the first frame's previous backlog is the last frame's, which the seam
    // check made equal to the backlog at 24:00.
    for (const row of queues(store)) {
      const last = store.nodeColumn(row, "backlog").at(-1) ?? 0;
      const { inflow, outflow } = bucketBalance(store, row, 0, last);
      expect(Math.abs(outflow - inflow)).toBeLessThanOrEqual(1e-4 * Math.abs(inflow) + 1e-3);
    }
  });
});
