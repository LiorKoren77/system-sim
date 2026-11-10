import { readFileSync } from "node:fs";
import path from "node:path";
import type { GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { layoutGraph } from "../src/diagram/layout";
import { toFlowElements, type NodeData } from "../src/diagram/to-flow-elements";
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
    const { nodes, edges } = toFlowElements(graph, await layoutGraph(graph), store, 0, null);
    expect(nodes.map((n) => n.id)).toEqual(["users", "api", "db"]);
    expect(edges.map((e) => [e.source, e.target])).toEqual([
      ["users", "api"],
      ["api", "db"],
    ]);
  });

  it("colours a node by its zone in the frame and writes the zone as a word", async () => {
    // chain's api at 1500/s on 2 × 1000/s: utilisation 0.75, above yellow (0.7), below red (0.9).
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), store, 0, null);
    expect(nodes.find((n) => n.id === "api")?.className).toContain("zone-yellow");
    expect(dataOf(nodes, "api")).toMatchObject({ zone: "yellow", rhoPercent: 75, replicas: 2 });
    expect(dataOf(nodes, "db")).toMatchObject({ zone: "green", rhoPercent: 36 });
  });

  it("shows a client with what it offers, the sum of its ops, and no zone", async () => {
    // chain's users: read 1200/s + write 300/s.
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), store, 0, null);
    expect(nodes.find((n) => n.id === "users")?.className).toContain("client");
    expect(dataOf(nodes, "users")).toMatchObject({ zone: null, offeredPerSecond: 1500 });
  });

  it("marks the selected node", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), store, 0, "db");
    expect(nodes.find((n) => n.id === "db")?.selected).toBe(true);
    expect(nodes.find((n) => n.id === "api")?.selected).toBe(false);
  });

  it("puts each node of a two-datacenter system in its datacenter's box", async () => {
    const layout = await layoutGraph(youtube);
    const { nodes } = toFlowElements(youtube, layout, null, 0, null);
    const groups = nodes.filter((n) => n.type === "group");
    expect(groups.map((g) => g.id)).toEqual(["dc:us-east", "dc:eu-west"]);
    expect(nodes.find((n) => n.id === "cdn_eu")?.parentId).toBe("dc:eu-west");
    expect(nodes.find((n) => n.id === "api")?.parentId).toBe("dc:us-east");
  });

  it("draws no datacenter box for a system in one datacenter", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    const { nodes } = toFlowElements(graph, await layoutGraph(graph), store, 0, null);
    expect(nodes.some((n) => n.type === "group")).toBe(false);
  });

  it("marks a bandwidth link as a pipe and a plain link not", async () => {
    const { edges } = toFlowElements(youtube, await layoutGraph(youtube), null, 0, null);
    const pipe = edges.find((e) => e.source === "cdn_eu" && e.target === "transcoded_store");
    expect(pipe?.className).toContain("pipe");
    const plain = edges.find((e) => e.source === "lb" && e.target === "api");
    expect(plain?.className ?? "").not.toContain("pipe");
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
