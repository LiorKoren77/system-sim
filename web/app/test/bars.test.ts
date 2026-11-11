import { DatasetStore, MemorySource } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { peaksOf } from "../src/diagram/bars";
import { nodeStatesAt } from "../src/diagram/to-flow-elements";
import { overloadedLines } from "../../stream/test/support/streams";
import { fixtureStore } from "./support";

describe("the bars in a box", () => {
  it("fills a node's busy bar with its mean utilisation and marks its peak", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(nodeStatesAt(graph, store, 0).get("api")?.busy).toEqual({ mean: 0.75, peak: 0.75 });
  });

  it("fills a client's load bar against its own busiest frame: a surge fills it", async () => {
    // chain's users offer a constant 1500/s, their own peak: a full bar.
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(nodeStatesAt(graph, store, 0).get("users")?.load).toBe(1);
    expect(nodeStatesAt(graph, store, 0).get("api")?.load).toBeNull();
  });

  it("fills a queue bar against the node's own largest backlog, as the queue builds and drains", async () => {
    // A 1000/s service sent 1500/s, 0.1 s buckets: the backlog grows 50 a bucket to its room, 100.
    const store = new DatasetStore();
    await store.consume(new MemorySource(overloadedLines()));
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const peaks = peaksOf(graph, store);
    expect(nodeStatesAt(graph, store, 0, peaks).get("api")?.queues).toEqual([
      { fill: 0.5, backlog: 50, consumer: null, zone: null },
    ]);
    expect(nodeStatesAt(graph, store, 5, peaks).get("api")?.queues).toEqual([
      { fill: 1, backlog: 100, consumer: null, zone: null },
    ]);
  });

  it("draws no queue bar for a node that never queues in the dataset", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(nodeStatesAt(graph, store, 0).get("api")?.queues).toEqual([]);
  });
});
