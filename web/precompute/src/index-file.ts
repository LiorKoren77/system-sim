// index.json: the list of datasets the page's catalogue shows (phase 5b spec §6). Sorted by id and
// free of time stamps, so its bytes depend only on the data, not on which run finished first.
import { FORMAT_VERSION } from "@simsys/stream";

export const INDEX_VERSION = 1;

export interface IndexEntry {
  readonly id: string;
  readonly file: string;
  readonly bytes: number;
  readonly title: string | null;
  readonly chapter: number | null;
  readonly kind: "day" | "run";
  readonly loops: boolean;
  readonly seam: "repeats" | "drifts" | null;
  readonly frames: number;
  readonly slo_passed: boolean;
}

export function indexJson(entries: readonly IndexEntry[], simsysCommit: string): string {
  const datasets = [...entries].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const index = {
    index_version: INDEX_VERSION,
    format_version: FORMAT_VERSION,
    simsys_commit: simsysCommit,
    datasets,
  };
  return JSON.stringify(index, null, 2) + "\n";
}
