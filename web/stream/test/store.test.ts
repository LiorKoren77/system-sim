import { describe, expect, it } from "vitest";
import { MemorySource } from "../src/memory-source";
import { bucketBalance, DatasetStore } from "../src/store";
import { helloLines } from "./support/hello";
import { helloWith, overloadedLines } from "./support/streams";

async function storeOf(lines: readonly string[], batchFrames = 64): Promise<DatasetStore> {
  const store = new DatasetStore({ batchFrames });
  await store.consume(new MemorySource(lines));
  return store;
}

describe("the dataset store", () => {
  it("holds hello's two frames as columns, a null as NaN", async () => {
    const store = await storeOf(helloLines);
    expect(store.status).toBe("ready");
    expect(store.frameCount).toBe(2);
    expect([...store.times()]).toEqual([0, 30]);
    expect([...store.durations()]).toEqual([30, 30]);
    expect([...store.nodeColumn(1, "mean_rho")]).toEqual([0.1, 0.1]);
    expect([...store.nodeColumn(0, "mean_rho")]).toEqual([NaN, NaN]);
    expect([...store.opColumn(0, "p99_ms")]).toEqual([5.66029, 5.66029]);
    expect(store.graph?.nodes.map((n) => n.id)).toEqual(["users", "api"]);
    expect(store.summary?.exit_code).toBe(0);
  });

  it("calls a stream that ends without its summary truncated: the run was killed", async () => {
    const store = await storeOf(helloLines.slice(0, 3));
    expect(store.status).toBe("truncated");
    expect(store.frameCount).toBe(1);
  });

  it("drops a partial last line and calls the stream truncated, not broken", async () => {
    const store = await storeOf([...helloLines.slice(0, 3), '{"type":"frame","rev":0,"t_s":3']);
    expect(store.status).toBe("truncated");
    expect(store.frameCount).toBe(1);
  });

  it("ends on an error line with its file, line, column and message", async () => {
    const store = await storeOf([
      ...helloLines.slice(0, 2),
      '{"type":"error","file":"x.yaml","line":12,"column":5,"message":"x.yaml:12:5: error: bad"}',
    ]);
    expect(store.status).toBe("error");
    expect(store.problem).toEqual({
      file: "x.yaml",
      line: 12,
      column: 5,
      message: "x.yaml:12:5: error: bad",
    });
  });

  it("refuses a frame for a graph revision that never came", async () => {
    const lines = [...helloLines];
    lines[2] = (lines[2] ?? "").replace('"rev":0', '"rev":1');
    const store = await storeOf(lines);
    expect(store.status).toBe("error");
    expect(store.problem?.message).toBe("frame for graph rev 1, which never came");
  });

  it("fails on a broken line in the middle, which no killed run explains", async () => {
    const store = await storeOf([helloLines[0] ?? "", "not json", ...helloLines.slice(1)]);
    expect(store.status).toBe("error");
    expect(store.problem?.message).toBe("line 2 is not JSON");
  });

  it("tells its subscribers once per batch of frames, not once per frame", async () => {
    const lines = helloWith(720, [100, 100, 0, 0, 0, 0, 0.1, 0.1, 0, 0, 1, null, null, null, null]);
    const store = new DatasetStore({ batchFrames: 64 });
    let calls = 0;
    store.subscribe(() => ++calls);
    await store.consume(new MemorySource(lines));
    expect(store.frameCount).toBe(720);
    expect(calls).toBeLessThanOrEqual(Math.ceil(720 / 64) + 2);
    expect(calls).toBeGreaterThan(1); // the page fills while it loads
  });

  it("keeps the conservation law of every bucket of an overloaded queue", async () => {
    // A bucket's balance (5a §6.2): demand · dt = (drain + dropped) · dt + backlog − the previous
    // frame's backlog. Rates are bucket means and the backlog a level, so it holds frame by frame.
    const store = await storeOf(overloadedLines());
    for (let k = 0; k < store.frameCount; ++k) {
      const { inflow, outflow } = bucketBalance(store, 1, k);
      expect(outflow).toBeCloseTo(inflow, 6);
    }
    expect(store.nodeColumn(1, "backlog").at(-1)).toBe(100);
    expect(store.nodeColumn(1, "dropped_per_s").at(-1)).toBe(500);
  });

  it("can stop reading when the page no longer wants the dataset", async () => {
    const controller = new AbortController();
    controller.abort();
    const store = new DatasetStore();
    await store.consume(new MemorySource(helloLines), controller.signal);
    expect(store.status).toBe("aborted");
  });
});
