import { describe, expect, it } from "vitest";
import { sloRows } from "../src/report/slo-rows";
import { fixtureStore } from "./support";

describe("the SLO table's rows", () => {
  it("say each of kv-store's objectives, its bound, what was measured and the verdict", async () => {
    const store = await fixtureStore("labs-kv-store");
    const report = store.summary?.report;
    if (!report || !store.graph || !store.dataset) throw new Error("no report");
    expect(sloRows(report, store.graph, store.dataset)).toEqual([
      {
        metric: "p99 latency",
        target: "clients · put",
        bound: "≤ 20 ms",
        observed: "2.33 ms",
        status: "pass",
        note: null,
        window: null,
      },
      {
        metric: "utilization",
        target: "store",
        bound: "≤ 80%",
        observed: "61%",
        status: "pass",
        note: null,
        window: null,
      },
      {
        metric: "drop rate",
        target: "every node",
        bound: "≤ 1%",
        observed: "0%",
        status: "pass",
        note: null,
        window: null,
      },
    ]);
  });

  it("name a windowed SLO's worst window on the day's clock: url_db's hour from 14:00", async () => {
    const store = await fixtureStore("book-url-shortener");
    const report = store.summary?.report;
    if (!report || !store.graph || !store.dataset) throw new Error("no report");
    // window_start_s 136800 is 14:00 of day 2 (136800 − 86400 = 50400 s); the SLO's over is 3600 s.
    expect(sloRows(report, store.graph, store.dataset)[2]?.window).toBe("worst 1 h from 14:00");
  });

  it("show a skipped SLO as skipped, with nothing measured", async () => {
    const store = await fixtureStore("labs-kv-store");
    const report = store.summary?.report;
    if (!report || !store.graph || !store.dataset) throw new Error("no report");
    const skipped = {
      ...report,
      slo: [{ ...report.slo[0], observed: null, status: "skipped", note: "no queue ran" }],
    } as typeof report;
    expect(sloRows(skipped, store.graph, store.dataset)[0]).toMatchObject({
      observed: "—",
      status: "skipped",
      note: "no queue ran",
    });
  });
});
