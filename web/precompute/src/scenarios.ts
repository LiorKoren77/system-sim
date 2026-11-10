// The shipped scenarios (scenarios/<group>/<name>/scenario.yaml) and how each becomes a dataset:
// a 24 h scenario is a day, run for two days so its second day can loop (phase 5b spec §2, §7);
// any other is a run, played once.
import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import type { Diagram } from "@simsys/stream";
import { parse } from "yaml";

export interface ScenarioEntry {
  readonly id: string; // `book/chat-system`: the path under scenarios/ without /scenario.yaml
  readonly path: string; // relative to the repository root, as simsys is given it
  readonly kind: "day" | "run";
  readonly title: string | null;
  readonly chapter: number | null;
  // The scenario's diagram.yaml as written, unchecked: the precompute checks it against the graph
  // simsys resolves (diagram.ts). null when the scenario has none.
  readonly diagram: Diagram | null;
}

// Every scenario under `<repoRoot>/scenarios`, sorted by id.
export async function findScenarios(repoRoot: string): Promise<ScenarioEntry[]> {
  const root = path.join(repoRoot, "scenarios");
  const entries: ScenarioEntry[] = [];
  for (const group of (await readdir(root, { withFileTypes: true })).filter((d) =>
    d.isDirectory(),
  )) {
    const groupDir = path.join(root, group.name);
    for (const name of (await readdir(groupDir, { withFileTypes: true })).filter((d) =>
      d.isDirectory(),
    )) {
      const dir = path.join(groupDir, name.name);
      const yamlText = await readFile(path.join(dir, "scenario.yaml"), "utf8").catch(() => null);
      if (yamlText === null) continue;
      const readme = await readFile(path.join(dir, "README.md"), "utf8").catch(() => "");
      const diagramText = await readFile(path.join(dir, "diagram.yaml"), "utf8").catch(() => null);
      entries.push({
        id: `${group.name}/${name.name}`,
        path: `scenarios/${group.name}/${name.name}/scenario.yaml`,
        kind: isDay(yamlText) ? "day" : "run",
        ...readmeHeading(readme),
        diagram: diagramText === null ? null : (parse(diagramText) as Diagram),
      });
    }
  }
  return entries.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
}

// The arguments simsys runs a scenario with: a day for two days (`run --days`, spec §5) in buckets
// of 2 min, 720 a day; a run as it is, in simsys's default 720 buckets.
export function simsysArgs(entry: ScenarioEntry): string[] {
  const base = ["run", entry.path, "--stream"];
  return entry.kind === "day" ? [...base, "--days", "2", "--every", "2m"] : base;
}

// The README's first heading, `# Chat system (ch. 12)`: the title, and the book's chapter when the
// heading names one.
export function readmeHeading(readme: string): { title: string | null; chapter: number | null } {
  const heading = /^# (.+)$/m.exec(readme)?.[1]?.trim();
  if (heading === undefined) return { title: null, chapter: null };
  const chapter = /^(.*?)\s*\(ch\. (\d+)\)$/.exec(heading);
  if (chapter?.[1] === undefined || chapter[2] === undefined)
    return { title: heading, chapter: null };
  return { title: chapter[1], chapter: Number(chapter[2]) };
}

// Whether the scenario's `duration` is one day. The one key the precompute reads from a scenario;
// the shipped files write it as a number and one unit (`24h`). A duration written another way is
// taken for a run: the page then plays it once instead of looping it, and simsys itself refuses
// `--days` on a scenario that is not a day.
function isDay(yamlText: string): boolean {
  const scenario = parse(yamlText) as { duration?: unknown } | null;
  const duration = scenario?.duration;
  if (typeof duration !== "string") return false;
  const match = /^(\d+)(d|h|m|s)$/.exec(duration);
  if (!match?.[1] || !match[2]) return false;
  const seconds = { d: 86400, h: 3600, m: 60, s: 1 }[match[2] as "d" | "h" | "m" | "s"];
  return Number(match[1]) * seconds === 86400;
}
