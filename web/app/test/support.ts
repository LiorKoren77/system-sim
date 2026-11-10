// The committed fixtures (web/stream/test/fixtures), as the app's tests read them.
import { readFileSync } from "node:fs";
import { gunzipSync } from "node:zlib";
import { DatasetStore, MemorySource } from "@simsys/stream";
import type { Index, IndexEntry } from "../src/catalogue/index-file";

export function fixtureLines(name: string): string[] {
  const gz = readFileSync(new URL(`../../stream/test/fixtures/${name}.ndjson.gz`, import.meta.url));
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
