// Through the real simsys binary (SIMSYS=<path to a Release simsys>; skipped without it): CI's web
// job runs it with the release job's binary.
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { precomputeOne } from "../src/precompute";
import { findScenarios } from "../src/scenarios";

const simsys = process.env.SIMSYS;
const repoRoot = fileURLToPath(new URL("../../..", import.meta.url));

describe.skipIf(simsys === undefined)("the precompute through the real simsys", () => {
  it("finds url-shortener's day repeating, keeps 720 frames, and reports the second day", async () => {
    const entry = (await findScenarios(repoRoot)).find((s) => s.id === "book/url-shortener");
    if (!entry || simsys === undefined) throw new Error("url-shortener or SIMSYS missing");
    const result = await precomputeOne(entry, { simsys, repoRoot });
    expect(result.index.seam).toBe("repeats");
    expect(result.index.loops).toBe(true);
    expect(result.index.frames).toBe(720);
    const report: unknown = JSON.parse(
      execFileSync(simsys, ["run", entry.path, "--days", "2", "--report", "json"], {
        cwd: repoRoot,
        encoding: "utf8",
      }),
    );
    expect(result.summary.report).toEqual(report);
  }, 60_000);
});
