// A dataset file (phase 5b spec §6): the `dataset` line, then simsys's own run, graph, kept frames
// and summary lines unchanged, gzipped. Nothing in it depends on when or where it was written, so
// the same simsys output gives the same bytes (Node's gzip writes a zero mtime).
import { gzipSync } from "node:zlib";
import { DATASET_VERSION, type DatasetLine, type GraphLine } from "@simsys/stream";
import type { ScenarioEntry } from "./scenarios";
import type { Window } from "./window";

export interface DatasetFile {
  readonly line: DatasetLine;
  readonly text: string;
  readonly gz: Uint8Array;
}

export function datasetFileName(id: string): string {
  return `${id.replaceAll("/", "-")}.ndjson.gz`;
}

export function buildDataset(
  entry: ScenarioEntry,
  stream: {
    readonly run: { readonly text: string };
    readonly graph: { readonly text: string; readonly message: GraphLine };
    readonly summary: { readonly text: string };
  },
  cut: Window,
  args: readonly string[],
): DatasetFile {
  // The keys in the order the spec lists them: JSON.stringify keeps an object's insertion order.
  const line: DatasetLine = {
    type: "dataset",
    dataset_version: DATASET_VERSION,
    id: entry.id,
    title: entry.title,
    chapter: entry.chapter,
    kind: entry.kind,
    window: cut.window,
    loops: cut.loops,
    seam: cut.seam,
    source: { scenario: entry.path, args },
  };
  const lines = [
    JSON.stringify(line),
    stream.run.text,
    stream.graph.text,
    ...cut.frameTexts,
    stream.summary.text,
  ];
  const text = lines.join("\n") + "\n";
  return { line, text, gz: gzipSync(text, { level: 9 }) };
}
