// A worker pool pulls from the queue it consumes: on that link the caller is the pool. The arrowhead
// points from caller to callee; the dashes move the way the data goes (queue to pool); and the
// messages the pool has not read yet are drawn in the queue's box, where they wait.
import { readFileSync } from "node:fs";
import path from "node:path";
import type { GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { layoutGraph } from "../src/diagram/layout";
import { nodeStatesAt, toFlowElements } from "../src/diagram/to-flow-elements";
import { queueAndPool } from "./support";

const youtube = {
  type: "graph",
  rev: 0,
  ...(JSON.parse(
    readFileSync(path.resolve(import.meta.dirname, "fixtures/youtube-graph.json"), "utf8"),
  ) as object),
} as GraphLine;

describe("a link a worker pool pulls from", () => {
  it("points its arrowhead at the queue, the callee, and no other link does", async () => {
    const { edges } = toFlowElements(youtube, await layoutGraph(youtube), null);
    const pull = edges.find((e) => e.source === "transcode_q" && e.target === "transcoders");
    expect(pull?.markerStart).toMatchObject({ type: "arrowclosed" });
    expect(pull?.markerEnd).toBeUndefined();
    const call = edges.find((e) => e.source === "lb" && e.target === "api");
    expect(call?.markerEnd).toMatchObject({ type: "arrowclosed" });
    expect(call?.markerStart).toBeUndefined();
  });

  it("still names the queue as the link's source, so the dashes move from the queue to the pool", async () => {
    const { edges } = toFlowElements(youtube, await layoutGraph(youtube), null);
    const pull = edges.find((e) => e.target === "transcoders");
    expect(pull?.source).toBe("transcode_q");
  });
});

describe("the messages a pool has not read yet", () => {
  it("are drawn in the queue's box, named after the pool that will read them", async () => {
    const store = await queueAndPool(2_900_000, 225);
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const states = nodeStatesAt(graph, store, 0);
    // 2.9M against the test pool's room of 1000: past 90%, red.
    expect(states.get("q")?.queues).toEqual([
      { fill: 1, backlog: 2_900_000, consumer: "w", zone: "red" },
    ]);
    expect(states.get("w")?.queues).toEqual([]);
  });

  it("come with the queue's lag: how long a message now published waits for a worker", async () => {
    const store = await queueAndPool(2_900_000, 225);
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(nodeStatesAt(graph, store, 0).get("q")?.lagSeconds).toBe(225);
  });

  it("draw no row while the pool has never fallen behind", async () => {
    const store = await queueAndPool(0, 0);
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(nodeStatesAt(graph, store, 0).get("q")?.queues).toEqual([]);
  });
});

describe("the colour of a queue's waiting messages", () => {
  it("follows the queue's lag against its SLO", async () => {
    // 75% of 5 min; 500 of the pool's room of 1000 keeps the room green, so the lag decides.
    const store = await queueAndPool(500, 225, 300);
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const q = nodeStatesAt(graph, store, 0).get("q");
    expect(q?.lagZone).toBe("yellow");
    expect(q?.queues[0]?.zone).toBe("yellow");
  });

  it("stays uncoloured for a queue no SLO judges, far from its room", async () => {
    const store = await queueAndPool(750, 225);
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const q = nodeStatesAt(graph, store, 0).get("q");
    expect(q?.lagZone).toBeNull();
    // 750 of the pool's room of 1000, 75%: yellow for the room, whatever the lag.
    expect(q?.queues[0]?.zone).toBe("yellow");
  });
});
