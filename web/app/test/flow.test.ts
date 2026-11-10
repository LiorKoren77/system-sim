import { describe, expect, it } from "vitest";
import { flowDuration, linkFlowsAt, busiestFlow } from "../src/diagram/flow";
import { fixtureStore } from "./support";

describe("the traffic a link carries in a frame", () => {
  it("is what its target box serves: chain's api drains 1500/s, its db 1800/s", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    // users → api, api → db
    expect(linkFlowsAt(graph, store, 0)).toEqual([1500, 1800]);
  });

  it("is the busiest any link carries over the whole dataset, for the speed scale", async () => {
    const store = await fixtureStore("tutorial-chain");
    const graph = store.graph;
    if (!graph) throw new Error("no graph");
    expect(busiestFlow(graph, store)).toBe(1800);
  });
});

describe("how fast a link's dashes move", () => {
  it("does not move a link that carries nothing", () => {
    expect(flowDuration(0, 1000)).toBeNull();
    expect(flowDuration(Number.NaN, 1000)).toBeNull();
  });

  it("moves the busiest link fastest, a dash cycle in half a second", () => {
    expect(flowDuration(1000, 1000)).toBe(0.5);
  });

  it("moves a busier link faster, on a logarithmic scale so a quiet link still moves", () => {
    const quiet = flowDuration(50, 40_000);
    const busy = flowDuration(20_000, 40_000);
    if (quiet === null || busy === null) throw new Error("both carry traffic");
    expect(busy).toBeLessThan(quiet);
    expect(quiet).toBeLessThanOrEqual(2.5);
  });

  it("changes in steps of a tenth of a second, so a small change of rate does not restart the dashes", () => {
    expect(flowDuration(1000, 1000)).toBe(flowDuration(990, 1000));
  });
});
