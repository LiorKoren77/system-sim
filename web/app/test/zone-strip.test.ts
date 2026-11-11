import { describe, expect, it } from "vitest";
import { frameAtX, rowAtY, zoneGrid } from "../src/charts/zone-strip";
import { fixtureStore, queueAndPool } from "./support";

describe("the zone strip's grid", () => {
  it("has a row per node but the clients, a cell per frame, each the frame's zone", async () => {
    const store = await fixtureStore("labs-kv-store");
    if (!store.graph) throw new Error("no graph");
    const grid = zoneGrid(store.graph, store);
    expect(grid.rows.map((r) => r.id)).toEqual(["coordinator", "store"]);
    expect(grid.frames).toBe(720);
    expect(grid.cells.length).toBe(2 * 720);
    const zones = store.nodeColumn(2, "zone");
    for (const k of [0, 240, 719]) expect(grid.cells[720 + k]).toBe(zones[k]);
  });
});

describe("a click on the strip", () => {
  it("lands on the frame under it: floor(x / width × frames), kept inside the day", () => {
    expect(frameAtX(50, 100, 720)).toBe(360);
    expect(frameAtX(0.1, 100, 720)).toBe(0);
    expect(frameAtX(100, 100, 720)).toBe(719);
    expect(frameAtX(-3, 100, 720)).toBe(0);
  });

  it("names the row under it, or none below the last", () => {
    expect(rowAtY(0, 8, 2)).toBe(0);
    expect(rowAtY(9, 8, 2)).toBe(1);
    expect(rowAtY(16, 8, 2)).toBeNull();
  });
});

describe("a queue's row in the strip", () => {
  it("is the zone its box shows: the lag against its SLO, not only the broker's", async () => {
    // 240 s of lag against a queue_lag SLO of 300 s: 80% of the bound, yellow; the broker is idle.
    const store = await queueAndPool(0, 240, 300);
    if (!store.graph) throw new Error("no graph");
    const grid = zoneGrid(store.graph, store);
    const q = grid.rows.findIndex((r) => r.id === "q");
    expect(store.nodeColumn(1, "zone")[0]).toBe(0);
    expect(grid.cells[q * grid.frames]).toBe(1);
  });
});
