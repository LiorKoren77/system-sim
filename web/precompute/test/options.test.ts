import { describe, expect, it } from "vitest";
import { readOptions } from "../src/options";

describe("the precompute's command line", () => {
  it("runs as many scenarios at once as --jobs says", () => {
    expect(readOptions(["--jobs", "3"], "out").jobs).toBe(3);
  });

  it("refuses a --jobs that is not a positive whole number, rather than run nothing", () => {
    for (const jobs of ["four", "0", "-2", "1.5", ""]) {
      expect(() => readOptions([`--jobs=${jobs}`], "out")).toThrow(
        `--jobs must be a positive whole number, not '${jobs}'`,
      );
    }
  });
});
