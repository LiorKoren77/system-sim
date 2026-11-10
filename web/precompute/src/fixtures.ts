// `npm run fixtures [-- --check]`: the datasets the tests read (web/stream/test/fixtures), written by
// the precompute with the real simsys. `--check` writes them to a temporary directory instead and
// compares their inflated text with the committed ones, so CI notices when simsys's output changed
// and the fixtures (the web code's picture of the contract) are stale. Inflated text, not the gzip
// bytes: another zlib may compress the same text differently.
import { mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { gunzipSync } from "node:zlib";
import { datasetFileName } from "./dataset";
import { readOptions, repoRoot } from "./options";
import { inPool, precomputeOne } from "./precompute";
import { findScenarios } from "./scenarios";

// The smallest streams, one with a fault and its block, and one real day.
const fixtureIds = ["tutorial/hello", "tutorial/chain", "labs/kv-store", "book/url-shortener"];
const committed = path.join(repoRoot, "web/stream/test/fixtures");

const options = readOptions(process.argv.slice(2), committed);
const out = options.check ? await mkdtemp(path.join(tmpdir(), "simsys-fixtures-")) : options.out;
const entries = (await findScenarios(repoRoot)).filter((s) => fixtureIds.includes(s.id));
await inPool(entries, options.jobs, async (entry) => {
  const result = await precomputeOne(entry, { simsys: options.simsys, repoRoot });
  await writeFile(path.join(out, datasetFileName(entry.id)), result.dataset.gz);
});

if (!options.check) {
  console.log(`fixtures: ${entries.length} written to ${out}`);
} else {
  let stale = 0;
  for (const entry of entries) {
    const name = datasetFileName(entry.id);
    const fresh = gunzipSync(await readFile(path.join(out, name))).toString("utf8");
    const old = await readFile(path.join(committed, name))
      .then((gz) => gunzipSync(gz).toString("utf8"))
      .catch(() => "");
    if (fresh === old) continue;
    ++stale;
    const freshLines = fresh.split("\n");
    const oldLines = old.split("\n");
    const at = freshLines.findIndex((line, i) => line !== oldLines[i]);
    console.error(`${name}: differs from simsys's output at line ${at + 1}`);
  }
  if (stale > 0) {
    console.error(`fixtures: ${stale} stale; run npm run fixtures and commit them`);
    process.exit(1);
  }
  console.log(`fixtures: ${entries.length} match simsys's output`);
}
