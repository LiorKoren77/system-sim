import { readFileSync } from "node:fs";
import path from "node:path";
import type { GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { layoutGraph } from "../src/diagram/layout";
import {
  nodeStatesAt,
  pipeZonesAt,
  toFlowElements,
  withPipeZones,
  type NodeData,
} from "../src/diagram/to-flow-elements";
import { fixtureStore } from "./support";

// youtube's resolved graph (`simsys explain scenarios/book/youtube/scenario.yaml --report json`),
// as a stream's graph line: two datacenters, a bandwidth link.
const youtube = {
  type: "graph",
  rev: 0,
  ...(JSON.parse(
    readFileSync(path.resolve(import.meta.dirname, "fixtures/youtube-graph.json"), "utf8"),
  ) as object),
} as GraphLine;

function dataOf(nodes: { id: string; data: unknown }[], id: string): NodeData {
  const node = nodes.find((n) => n.id === id);
  if (!node) throw new Error(`no node ${id}`);
  return node.data as NodeData;
}

describe("the diagram's elements, from the graph and the frame", () => {
  it("draws chain's three nodes and two links in the graph's order", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes, edges } = toFlowElements(graph, await layoutGraph(graph), null);
    expect(nodes.map((n) => n.id)).toEqual(["users", "api", "db"]);
    expect(edges.map((e) => [e.source, e.target])).toEqual([
      ["users", "api"],
      ["api", "db"],
    ]);
    // A chain laid out left to right: every edge leaves a right side and enters a left side.
    expect(edges.map((e) => [e.sourceHandle, e.targetHandle])).toEqual([
      ["source-right", "target-left"],
      ["source-right", "target-left"],
    ]);
  });

  it("gives each node its zone, utilisation and, for a client, what it offers, per frame", async () => {
    // chain's api at 1500/s on 2 × 1000/s: utilisation 0.75, above yellow (0.7), below red (0.9);
    // db at 0.36; the users client offers read 1200/s + write 300/s.
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const states = nodeStatesAt(graph, store, 0);
    expect(states.get("api")).toEqual({ zone: "yellow", rhoPercent: 75, offeredPerSecond: null });
    expect(states.get("db")).toMatchObject({ zone: "green", rhoPercent: 36 });
    expect(states.get("users")).toEqual({ zone: null, rhoPercent: null, offeredPerSecond: 1500 });
  });

  it("keeps a node's copies and its client look in the structure, which no frame changes", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), null);
    expect(dataOf(nodes, "api")).toMatchObject({ type: "service", replicas: 2 });
    expect(nodes.find((n) => n.id === "users")?.className).toBe("client");
  });

  it("marks the selected node", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), "db");
    expect(nodes.find((n) => n.id === "db")?.selected).toBe(true);
    expect(nodes.find((n) => n.id === "api")?.selected).toBe(false);
  });

  it("puts each node of a two-datacenter system in its datacenter's box", async () => {
    const layout = await layoutGraph(youtube);
    const { nodes } = toFlowElements(youtube, layout, null);
    const groups = nodes.filter((n) => n.type === "group");
    expect(groups.map((g) => g.id)).toEqual(["dc:us-east", "dc:eu-west"]);
    expect(nodes.find((n) => n.id === "cdn_eu")?.parentId).toBe("dc:eu-west");
    expect(nodes.find((n) => n.id === "api")?.parentId).toBe("dc:us-east");
  });

  it("draws no datacenter box for a system in one datacenter", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), null);
    expect(nodes.some((n) => n.type === "group")).toBe(false);
  });

  it("marks a bandwidth link as a pipe and a plain link not", async () => {
    const { edges } = toFlowElements(youtube, await layoutGraph(youtube), null);
    const pipe = edges.find((e) => e.source === "cdn_eu" && e.target === "transcoded_store");
    expect(pipe?.className).toBe("pipe");
    const plain = edges.find((e) => e.source === "lb" && e.target === "api");
    expect(plain?.className).toBeUndefined();
  });

  it("colours a pipe by its zone in the frame, and leaves a plain link alone", async () => {
    const { edges } = toFlowElements(youtube, await layoutGraph(youtube), null);
    const zones = youtube.links.map((link) => (link.pipe_row === null ? null : ("red" as const)));
    const coloured = withPipeZones(edges, zones);
    const at = youtube.links.findIndex((l) => l.from === "cdn_eu" && l.to === "transcoded_store");
    expect(coloured[at]?.className).toBe("pipe zone-red");
    const plain = youtube.links.findIndex((l) => l.from === "lb" && l.to === "api");
    expect(coloured[plain]).toBe(edges[plain]); // the same object: React Flow redraws nothing
  });

  it("reads no pipe zone without a store, and one per link otherwise", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(pipeZonesAt(graph, store, 0)).toEqual([null, null]); // chain has no bandwidth link
    expect(pipeZonesAt(youtube, null, 0).every((z) => z === null)).toBe(true);
  });
});

describe("the diagram's layout", () => {
  it("places every node, clients first from the left", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const layout = await layoutGraph(graph);
    const x = (id: string) => layout.nodes.get(id)?.x ?? NaN;
    for (const node of graph.nodes) expect(Number.isFinite(x(node.id))).toBe(true);
    expect(x("users")).toBeLessThan(x("api"));
    expect(x("api")).toBeLessThan(x("db"));
  });

  it("places youtube's fourteen nodes, the same way every time", async () => {
    const first = await layoutGraph(youtube);
    const second = await layoutGraph(youtube);
    expect(first.nodes.size).toBe(14);
    expect([...first.nodes]).toEqual([...second.nodes]);
  });
});
