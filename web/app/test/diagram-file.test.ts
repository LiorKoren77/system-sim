// A scenario's diagram.yaml on the page: boxes on the cells it names, links on the sides it names.
import { readFileSync } from "node:fs";
import path from "node:path";
import type { GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { absoluteBox, CELL_HEIGHT, CELL_WIDTH, layoutGraph } from "../src/diagram/layout";
import { toFlowElements } from "../src/diagram/to-flow-elements";
import { fixtureStore } from "./support";

async function chain(): Promise<GraphLine> {
  const graph = (await fixtureStore("tutorial-chain")).graph;
  if (!graph) throw new Error("no graph");
  return graph;
}

const youtube = {
  type: "graph",
  rev: 0,
  ...(JSON.parse(
    readFileSync(path.resolve(import.meta.dirname, "fixtures/youtube-graph.json"), "utf8"),
  ) as object),
} as GraphLine;

describe("a diagram placed by hand", () => {
  it("puts each box on its cell of the grid", async () => {
    const layout = await layoutGraph(await chain(), {
      cells: { users: [0, 0], api: [1, 1], db: [2, 0] },
    });
    expect(absoluteBox(layout, "users")).toMatchObject({ x: 0, y: 0 });
    expect(absoluteBox(layout, "api")).toMatchObject({ x: CELL_WIDTH, y: CELL_HEIGHT });
    expect(absoluteBox(layout, "db")).toMatchObject({ x: 2 * CELL_WIDTH, y: 0 });
  });

  it("joins the facing sides of placed boxes, or the sides the file names", async () => {
    const layout = await layoutGraph(await chain(), {
      cells: { users: [0, 0], api: [0, 1], db: [1, 1] },
      sides: { "api -> db": ["bottom", "bottom"] },
    });
    expect(layout.sides.get("link-0")).toEqual({ source: "bottom", target: "top" }); // api is below users
    expect(layout.sides.get("link-1")).toEqual({ source: "bottom", target: "bottom" });
  });

  it("draws right-angled lines between placed boxes, which ELK did not route", async () => {
    const graph = await chain();
    const layout = await layoutGraph(graph, { cells: { users: [0, 0], api: [1, 0], db: [2, 0] } });
    expect(layout.routes.size).toBe(0);
    const { edges } = toFlowElements(graph, layout, null);
    expect(edges.map((e) => [e.data?.points, e.sourceHandle, e.targetHandle])).toEqual([
      [undefined, "source-right", "target-left"],
      [undefined, "source-right", "target-left"],
    ]);
  });

  it("draws each datacenter as a box around its nodes' cells", async () => {
    const cells = Object.fromEntries(
      youtube.nodes.map((n, i) => [
        n.id,
        [n.datacenter === "eu-west" ? 0 : 1 + (i % 4), Math.floor(i / 4)],
      ]),
    ) as Record<string, [number, number]>;
    const layout = await layoutGraph(youtube, { cells });
    for (const node of youtube.nodes) {
      const box = absoluteBox(layout, node.id);
      const dc = layout.groups.get(`dc:${node.datacenter}`);
      if (!box || !dc) throw new Error(node.id);
      expect(box.x).toBeGreaterThanOrEqual(dc.x);
      expect(box.y).toBeGreaterThanOrEqual(dc.y);
      expect(box.x + box.width).toBeLessThanOrEqual(dc.x + dc.width);
      expect(box.y + box.height).toBeLessThanOrEqual(dc.y + dc.height);
    }
  });
});

describe("a diagram whose file names only sides", () => {
  it("lets ELK place the boxes and routes the link into the sides it names", async () => {
    const layout = await layoutGraph(await chain(), {
      sides: { "users -> api": ["bottom", "top"] },
    });
    expect(layout.sides.get("link-0")).toEqual({ source: "bottom", target: "top" });
    const users = absoluteBox(layout, "users");
    const route = layout.routes.get("link-0");
    if (!users || !route) throw new Error("no layout");
    expect(route[0]?.y).toBeCloseTo(users.y + users.height, 0);
  });
});
