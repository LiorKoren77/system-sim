import { describe, expect, it } from "vitest";
import { indexJson, type IndexEntry } from "../src/index-file";

function entry(id: string): IndexEntry {
  return {
    id,
    file: `${id.replace("/", "-")}.ndjson.gz`,
    bytes: 100,
    title: id,
    chapter: null,
    kind: "run",
    duration_s: 60,
    loops: false,
    seam: null,
    frames: 2,
    slo_passed: true,
  };
}

describe("the index of the datasets", () => {
  it("lists them sorted by id, whatever order their runs finished in", () => {
    const index = JSON.parse(
      indexJson(
        [entry("tutorial/hello"), entry("book/youtube"), entry("labs/kv-store")],
        "abc1234",
      ),
    ) as { datasets: { id: string }[] };
    expect(index.datasets.map((d) => d.id)).toEqual([
      "book/youtube",
      "labs/kv-store",
      "tutorial/hello",
    ]);
  });

  it("names its versions and the simsys commit that wrote the data, first", () => {
    const index = JSON.parse(indexJson([entry("tutorial/hello")], "abc1234")) as object;
    expect(Object.keys(index)).toEqual([
      "index_version",
      "format_version",
      "simsys_commit",
      "datasets",
    ]);
    expect(index).toMatchObject({ index_version: 1, format_version: 1, simsys_commit: "abc1234" });
  });
});
