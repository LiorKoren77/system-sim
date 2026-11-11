import { readFileSync } from "node:fs";
import path from "node:path";
import { DatasetStore, MemorySource, type GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { chartsFor, rateUnit, toAligned, xValues } from "../src/charts/series";
import { fixtureLines, fixtureStore } from "./support";

const youtube = {
  type: "graph",
  rev: 0,
  ...(JSON.parse(
    readFileSync(path.resolve(import.meta.dirname, "fixtures/youtube-graph.json"), "utf8"),
  ) as object),
} as GraphLine;

function chart(charts: ReturnType<typeof chartsFor>, key: string) {
  const found = charts.find((c) => c.key === key);
  if (!found) throw new Error(`no chart ${key} in ${charts.map((c) => c.key).join(", ")}`);
  return found;
}

function graphOf(store: DatasetStore): GraphLine {
  if (!store.graph) throw new Error("no graph");
  return store.graph;
}

// youtube's graph streamed in two frames of zeros but for its one bandwidth link: enough for the
// link's charts, which read the link's own columns.
async function youtubeStore(): Promise<DatasetStore> {
  const zeros = (columns: readonly string[]) => columns.map(() => 0);
  const frame = (t_s: number) => ({
    type: "frame",
    rev: 0,
    t_s,
    dt_s: 60,
    nodes: youtube.nodes.map(() => zeros(youtube.node_columns)),
    links: [[120, 120, 0, 0, 0, 0.024, 0.024, 0, 0, 75]],
    ops: youtube.ops.map(() => zeros(youtube.op_columns)),
  });
  const run = {
    type: "run",
    format_version: 1,
    scenario: "youtube",
    seed: 1,
    duration_s: 120,
    step_s: 1,
    warmup_s: 0,
    bucket_s: 60,
    frames: 2,
  };
  const store = new DatasetStore();
  await store.consume(
    new MemorySource([run, youtube, frame(0), frame(60)].map((line) => JSON.stringify(line))),
  );
  return store;
}

describe("a node's charts, as uPlot's inputs", () => {
  it("put the frames on the window's clock: a day's first frame at 0, not at 24:00", async () => {
    const day = await fixtureStore("book-url-shortener");
    expect(xValues(day, 86400)[0]).toBe(0);
    expect(xValues(day, 86400)[1]).toBe(120);
    const run = await fixtureStore("tutorial-chain");
    expect(Array.from(xValues(run, 0).subarray(0, 2))).toEqual([0, 0.2]);
  });

  it("show api's busy share, mean and peak, against its yellow 0.7 and red 0.9 thresholds", async () => {
    const store = await fixtureStore("tutorial-chain");
    const utilisation = chart(chartsFor(graphOf(store), store, "api"), "utilisation");
    expect(utilisation.series.map((s) => s.label)).toEqual([
      "mean",
      "peak",
      "yellow 70%",
      "red 90%",
    ]);
    expect(utilisation.series[0]?.values).toEqual(store.nodeColumn(1, "mean_rho"));
    expect(utilisation.series[1]?.values).toEqual(store.nodeColumn(1, "peak_rho"));
    expect(new Set(utilisation.series[2]?.values)).toEqual(new Set([0.7]));
    expect(new Set(utilisation.series[3]?.values)).toEqual(new Set([0.9]));
    expect(utilisation.series.map((s) => s.role)).toEqual(["data", "data", "yellow", "red"]);
  });

  it("show the backlog against the room, queue_limit 1000, as a line", async () => {
    const store = await fixtureStore("tutorial-chain");
    const backlog = chart(chartsFor(graphOf(store), store, "api"), "backlog");
    expect(backlog.series.map((s) => s.label)).toEqual(["waiting", "peak", "room 1000"]);
    expect(new Set(backlog.series[2]?.values)).toEqual(new Set([1000]));
  });

  it("give rates in the node's unit: units for a costed database, MB for a byte-sized CDN", async () => {
    const store = await fixtureStore("book-url-shortener");
    const rates = chart(chartsFor(graphOf(store), store, "url_db"), "rates");
    expect(rates.unit).toBe("units/s");
    expect(rates.series.map((s) => s.label)).toEqual(["demand", "served", "dropped", "rejected"]);
    const cdn = youtube.nodes.find((n) => n.id === "cdn_us");
    expect(cdn && rateUnit(cdn)).toBe("MB/s");
  });

  it("show a service the four charts every node has, and none of the columns its type lacks", async () => {
    const store = await fixtureStore("tutorial-chain");
    expect(chartsFor(graphOf(store), store, "api").map((c) => c.key)).toEqual([
      "utilisation",
      "backlog",
      "rates",
      "latency",
    ]);
  });

  it("add a column of the node's own type once it has a value: a gateway's connections", async () => {
    // chain with api holding 5000 open connections, as a ws_gateway would.
    const lines = fixtureLines("tutorial-chain").map((line) => {
      if (!line.includes('"type":"frame"')) return line;
      const frame = JSON.parse(line) as { nodes: (number | null)[][] };
      const api = frame.nodes[1];
      if (api) api[12] = 5000;
      return JSON.stringify(frame);
    });
    const store = new DatasetStore();
    await store.consume(new MemorySource(lines));
    const keys = chartsFor(graphOf(store), store, "api").map((c) => c.key);
    expect(keys).toContain("connections");
    expect(keys).not.toContain("queue_lag_s");
  });
});

describe("a client's charts", () => {
  it("are its ops' rates and latencies, the p99 labelled estimated as the graph says", async () => {
    const store = await fixtureStore("tutorial-chain");
    const charts = chartsFor(graphOf(store), store, "users");
    expect(charts.map((c) => c.key)).toEqual([
      "op-read-rates",
      "op-read-latency",
      "op-write-rates",
      "op-write-latency",
    ]);
    expect(chart(charts, "op-read-rates").series.map((s) => s.label)).toEqual([
      "offered",
      "answered",
      "timed out",
    ]);
    expect(chart(charts, "op-read-latency").series.map((s) => s.label)).toEqual([
      "mean",
      "p99 (estimated)",
    ]);
    expect(chart(charts, "op-read-latency").series[1]?.values).toEqual(store.opColumn(0, "p99_ms"));
  });
});

describe("a bandwidth link's charts", () => {
  it("are its pipe's columns, rates in MB/s, chosen by from->to", async () => {
    const store = await youtubeStore();
    const charts = chartsFor(graphOf(store), store, "cdn_eu->transcoded_store");
    expect(chart(charts, "rates").unit).toBe("MB/s");
    expect(Array.from(chart(charts, "rates").series[0]?.values ?? [])).toEqual([120, 120]);
    expect(charts.map((c) => c.key)).toEqual(["utilisation", "backlog", "rates", "latency"]);
  });

  it("are none for a link without a bandwidth, or an id the graph does not have", async () => {
    const store = await youtubeStore();
    expect(chartsFor(graphOf(store), store, "lb->api")).toEqual([]);
    expect(chartsFor(graphOf(store), store, "nobody")).toEqual([]);
  });
});

describe("the data handed to uPlot", () => {
  it("is the x column then each series, NaN written as null so uPlot draws a gap", () => {
    const x = new Float64Array([0, 1, 2]);
    const aligned = toAligned(x, [{ values: new Float64Array([1, NaN, 3]) }]);
    expect(aligned[0]).toBe(x);
    expect(aligned[1]).toEqual([1, null, 3]);
  });
});
