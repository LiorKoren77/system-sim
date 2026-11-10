// `npm run precompute [-- --simsys <path>] [--jobs N] [--only book/youtube,...] [--out <dir>]`:
// every shipped scenario (or those named) through simsys into web/app/public/datasets, then
// index.json (phase 5b spec §7). Offline: run it after a change to the models, then `npm run serve`.
import { execFileSync } from "node:child_process";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { datasetFileName } from "./dataset";
import { indexJson, mergeEntries, type IndexEntry } from "./index-file";
import { readOptions, repoRoot } from "./options";
import { inPool, precomputeOne, type Precomputed } from "./precompute";
import { findScenarios, type ScenarioEntry } from "./scenarios";

const options = readOptions(process.argv.slice(2), path.join(repoRoot, "web/app/public/datasets"));
const all = await findScenarios(repoRoot);
const chosen = options.only === null ? all : all.filter((s) => options.only?.includes(s.id));
await mkdir(options.out, { recursive: true });

console.log(
  `precompute: ${chosen.length} scenarios, ${options.jobs} at a time, with ${options.simsys}`,
);
const failures: string[] = [];
const results = await inPool(chosen, options.jobs, async (entry: ScenarioEntry) => {
  try {
    const result = await precomputeOne(entry, { simsys: options.simsys, repoRoot });
    await writeFile(path.join(options.out, datasetFileName(entry.id)), result.dataset.gz);
    console.log(describe(entry, result));
    return result;
  } catch (error) {
    failures.push(error instanceof Error ? error.message : String(error));
    console.error(`${entry.id}  FAILED`);
    return null;
  }
});

if (failures.length > 0) {
  for (const failure of failures) console.error(failure);
  console.error(`precompute: ${failures.length} failed; index.json not written`);
  process.exit(1);
}
const fresh = results.flatMap((r) => (r === null ? [] : [r.index]));
// With --only, the other datasets' entries stay in the index: a partial run must not empty the
// catalogue.
const index = options.only === null ? fresh : mergeEntries(await earlierEntries(), fresh);
await writeFile(path.join(options.out, "index.json"), indexJson(index, simsysCommit()));
console.log(`precompute: ${index.length} datasets and index.json in ${options.out}`);

function describe(entry: ScenarioEntry, result: Precomputed): string {
  const { index: i, seconds } = result;
  const status = i.kind === "day" ? (i.seam ?? "") : "run";
  const kb = (i.bytes / 1024).toFixed(0);
  return `${entry.id.padEnd(28)}${i.kind.padEnd(5)}${status.padEnd(9)}${String(i.frames).padStart(5)} frames ${kb.padStart(5)} KB ${seconds.toFixed(1).padStart(6)} s`;
}

// The commit of the simsys sources the datasets came from, `-dirty` when the tree has changes.
function simsysCommit(): string {
  const git = (args: string[]) =>
    execFileSync("git", args, { cwd: repoRoot, encoding: "utf8" }).trim();
  try {
    const commit = git(["rev-parse", "--short", "HEAD"]);
    return git(["status", "--porcelain", "--", "src", "scenarios"]) === ""
      ? commit
      : `${commit}-dirty`;
  } catch {
    return "unknown";
  }
}

// The entries of the index.json already in the output directory; none when there is none.
async function earlierEntries(): Promise<IndexEntry[]> {
  const text = await readFile(path.join(options.out, "index.json"), "utf8").catch(() => null);
  if (text === null) return [];
  return (JSON.parse(text) as { datasets: IndexEntry[] }).datasets;
}
