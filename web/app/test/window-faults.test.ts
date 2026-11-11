import type { FaultBlock } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { faultsInWindow, frameAt } from "../src/report/window-faults";
import { fixtureStore } from "./support";

const block = (at_s: number): FaultBlock => ({
  at_s,
  kind: "node_down",
  node: "chat_servers",
  for_s: 60,
  during: {},
  after: {},
});

describe("the faults the page shows", () => {
  it("are a day's own: chat-system's report also judges day 1's copy, which the page leaves out", () => {
    // `--days 2` repeats the 21:30 outage on both days (spec §5): at 77400 and 163800. The page
    // plays day 2, 86400-172800, so only the second is in the window.
    const shown = faultsInWindow([block(77400), block(163800)], { from_s: 86400, to_s: 172800 });
    expect(shown.map((f) => f.at_s)).toEqual([163800]);
  });

  it("are all of a run's", async () => {
    const store = await fixtureStore("labs-kv-store");
    const report = store.summary?.report;
    expect(faultsInWindow(report?.faults ?? [], { from_s: 0, to_s: 1800 })).toHaveLength(1);
  });
});

describe("the frame a fault starts in", () => {
  it("is the bucket its time falls in: kv-store's node_down at 600 s, buckets of 2.5 s", async () => {
    const store = await fixtureStore("labs-kv-store");
    expect(frameAt(store.times(), 600)).toBe(240);
    expect(frameAt(store.times(), 601)).toBe(240);
    expect(frameAt(store.times(), -5)).toBe(0);
    expect(frameAt(store.times(), 1e9)).toBe(719);
  });
});
