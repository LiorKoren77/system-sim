// The committed fixtures (web/stream/test/fixtures), as the app's tests read them.
import { readFileSync } from "node:fs";
import path from "node:path";
import { gunzipSync } from "node:zlib";
import { DatasetStore, MemorySource } from "@simsys/stream";
import { helloLines } from "../../stream/test/support/hello";
import type { Index, IndexEntry } from "../src/catalogue/index-file";

export function fixtureLines(name: string): string[] {
  // A path from this file's directory: under Vite, import.meta.url is a dev-server URL, not a file.
  const gz = readFileSync(
    path.resolve(import.meta.dirname, `../../stream/test/fixtures/${name}.ndjson.gz`),
  );
  return gunzipSync(gz)
    .toString("utf8")
    .split("\n")
    .filter((line) => line !== "");
}

export async function fixtureStore(name: string): Promise<DatasetStore> {
  const store = new DatasetStore();
  await store.consume(new MemorySource(fixtureLines(name)));
  return store;
}

export function entry(id: string, overrides: Partial<IndexEntry> = {}): IndexEntry {
  return {
    id,
    file: `${id.replace("/", "-")}.ndjson.gz`,
    bytes: 1000,
    title: id,
    chapter: null,
    kind: "run",
    duration_s: 1800,
    loops: false,
    seam: null,
    frames: 720,
    slo_passed: true,
    ...overrides,
  };
}

export function index(entries: IndexEntry[]): Index {
  return { index_version: 1, format_version: 1, simsys_commit: "test", datasets: entries };
}

// hello's stream with its graph replaced by publisher → queue q → pool w, w queueing `backlog`
// messages and q lagging `lag` seconds in the one frame.
export async function queueAndPool(
  backlog: number,
  lag: number,
  lagSlo?: number,
): Promise<DatasetStore> {
  const graph = JSON.parse(helloLines[1] ?? "") as Record<string, unknown>;
  const node = (id: string, type: string) => ({
    id,
    type,
    datacenter: "main",
    replicas: 1,
    shards: null,
    capacity_per_s: type === "client" ? null : 1000,
    unit: type === "client" ? null : "requests",
    yellow: type === "client" ? null : 0.7,
    red: type === "client" ? null : 0.9,
    queue_limit: type === "client" ? null : 1000,
  });
  const link = (from: string, to: string) => ({
    from,
    to,
    direction: "out",
    ops: [],
    one_way_s: 0,
    bandwidth_mb_s: null,
    pipe_row: null,
  });
  graph.nodes = [node("pub", "client"), node("q", "message_queue"), node("w", "worker_pool")];
  graph.links = [link("pub", "q"), link("q", "w")];
  graph.slos =
    lagSlo === undefined
      ? []
      : [{ metric: "queue_lag", node: "q", op: null, max: lagSlo, over_s: null }];
  const row = (b: number, l: number | null) => [
    500,
    500,
    0,
    0,
    b,
    b,
    0.02,
    0.02,
    0,
    0,
    1,
    l,
    null,
    null,
    null,
  ];
  const frame = {
    type: "frame",
    rev: 0,
    t_s: 0,
    dt_s: 60,
    nodes: [Array(15).fill(null), row(0, lag), row(backlog, null)],
    links: [],
    ops: [[500, 500, 500, 0, 1, 2]],
  };
  const store = new DatasetStore();
  await store.consume(
    new MemorySource([
      helloLines[0] ?? "",
      JSON.stringify(graph),
      JSON.stringify(frame),
      helloLines[4] ?? "",
    ]),
  );
  return store;
}
