import { gunzipSync } from "node:zlib";
import { describe, expect, it } from "vitest";
import { buildDataset, datasetFileName } from "../src/dataset";
import type { ScenarioEntry } from "../src/scenarios";
import type { Window } from "../src/window";
import { graph } from "./support";

const entry: ScenarioEntry = {
  id: "book/chat-system",
  path: "scenarios/book/chat-system/scenario.yaml",
  kind: "day",
  title: "Chat system",
  chapter: 12,
};

const stream = {
  run: { text: '{"type":"run"}' },
  graph: { text: '{"type":"graph"}', message: graph },
  summary: { text: '{"type":"summary"}' },
};

const cut: Window = {
  frames: [],
  frameTexts: ['{"type":"frame","t_s":86400}', '{"type":"frame","t_s":86520}'],
  window: { from_s: 86400, to_s: 172800 },
  loops: true,
  seam: { status: "repeats", worst: null },
};

describe("a dataset file", () => {
  it("is the dataset line, then simsys's run, graph, kept frames and summary, as written", () => {
    const dataset = buildDataset(entry, stream, cut, ["run", entry.path, "--stream"]);
    const lines = gunzipSync(dataset.gz).toString("utf8").split("\n");
    expect(lines.slice(1)).toEqual([
      '{"type":"run"}',
      '{"type":"graph"}',
      '{"type":"frame","t_s":86400}',
      '{"type":"frame","t_s":86520}',
      '{"type":"summary"}',
      "",
    ]);
    expect(Object.keys(JSON.parse(lines[0] ?? "") as object)).toEqual([
      "type",
      "dataset_version",
      "id",
      "title",
      "chapter",
      "kind",
      "window",
      "loops",
      "seam",
      "source",
    ]);
  });

  it("is the same bytes every time it is written: no time stamp, gzip's mtime 0", () => {
    const args = ["run", entry.path, "--stream"];
    expect(buildDataset(entry, stream, cut, args).gz).toEqual(
      buildDataset(entry, stream, cut, args).gz,
    );
  });

  it("is named after its id", () => {
    expect(datasetFileName("book/chat-system")).toBe("book-chat-system.ndjson.gz");
  });
});
