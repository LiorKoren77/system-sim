// index.json, written by the precompute (web/precompute/src/index-file.ts): the datasets the
// catalogue lists.

export interface IndexEntry {
  readonly id: string;
  readonly file: string;
  readonly bytes: number;
  readonly title: string | null;
  readonly chapter: number | null;
  readonly kind: "day" | "run";
  readonly duration_s: number;
  readonly loops: boolean;
  readonly seam: "repeats" | "drifts" | null;
  readonly frames: number;
  readonly slo_passed: boolean;
}

export interface Index {
  readonly index_version: number;
  readonly format_version: number;
  readonly simsys_commit: string;
  readonly datasets: readonly IndexEntry[];
}

export async function fetchIndex(url = "datasets/index.json"): Promise<Index> {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url}: ${response.status} ${response.statusText}`);
  return (await response.json()) as Index;
}

// The dataset the page opens when the address names none: the first day that loops, the page's
// reason to exist, else the first of all.
export function defaultEntry(index: Index): IndexEntry | undefined {
  return index.datasets.find((d) => d.loops) ?? index.datasets[0];
}
