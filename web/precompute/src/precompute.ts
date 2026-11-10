// One scenario into one dataset, and a pool that runs several at once.
import type { SummaryLine } from "@simsys/stream";
import { buildDataset, datasetFileName, type DatasetFile } from "./dataset";
import type { IndexEntry } from "./index-file";
import { readStream, runSimsys } from "./run-simsys";
import { simsysArgs, type ScenarioEntry } from "./scenarios";
import { cutWindow } from "./window";

export interface Precomputed {
  readonly dataset: DatasetFile;
  readonly index: IndexEntry;
  readonly summary: SummaryLine;
  readonly seconds: number; // how long simsys took, for the log (never written into a file)
}

export async function precomputeOne(
  entry: ScenarioEntry,
  options: { readonly simsys: string; readonly repoRoot: string },
): Promise<Precomputed> {
  const args = simsysArgs(entry);
  const started = performance.now();
  const stream = readStream(await runSimsys(options.simsys, args, options.repoRoot), entry.path);
  const seconds = (performance.now() - started) / 1000;
  const cut = cutWindow(
    entry.kind,
    stream.graph.message,
    stream.frames.map((f) => f.message),
    stream.run.message.duration_s,
    stream.frames.map((f) => f.text),
  );
  const dataset = buildDataset(entry, stream, cut, args);
  const index: IndexEntry = {
    id: entry.id,
    file: datasetFileName(entry.id),
    bytes: dataset.gz.length,
    title: entry.title,
    chapter: entry.chapter,
    kind: entry.kind,
    duration_s: stream.run.message.duration_s,
    loops: cut.loops,
    seam: cut.seam?.status ?? null,
    frames: cut.frames.length,
    slo_passed: stream.summary.message.exit_code === 0,
  };
  return { dataset, index, summary: stream.summary.message, seconds };
}

// Calls `work` on every item, at most `jobs` at a time, and returns the results in the items'
// order. Each simsys run takes one core, so `jobs` is the machine's core count by default.
export async function inPool<T, R>(
  items: readonly T[],
  jobs: number,
  work: (item: T) => Promise<R>,
): Promise<R[]> {
  const results: R[] = new Array<R>(items.length);
  let next = 0;
  const worker = async () => {
    while (next < items.length) {
      const i = next++;
      results[i] = await work(items[i] as T);
    }
  };
  await Promise.all(Array.from({ length: Math.max(1, Math.min(jobs, items.length)) }, worker));
  return results;
}
