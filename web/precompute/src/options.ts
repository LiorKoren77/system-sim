// The command line of `npm run precompute` and `npm run fixtures`, and the paths they default to.
import { availableParallelism } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

export const repoRoot = fileURLToPath(new URL("../../..", import.meta.url));

export interface Options {
  readonly simsys: string;
  readonly jobs: number;
  readonly only: readonly string[] | null; // ids; null: every scenario
  readonly out: string;
  readonly check: boolean;
}

// `text` as a whole number above 0, or an Error naming the option (Number("four") is NaN, and a
// pool of NaN workers runs nothing and writes an empty index).
function positiveWhole(option: string, text: string): number {
  const value = Number(text);
  if (text.trim() === "" || !Number.isInteger(value) || value < 1) {
    throw new Error(`${option} must be a positive whole number, not '${text}'`);
  }
  return value;
}

export function readOptions(argv: readonly string[], defaultOut: string): Options {
  const { values } = parseArgs({
    args: [...argv],
    options: {
      simsys: { type: "string" },
      jobs: { type: "string" },
      only: { type: "string" },
      out: { type: "string" },
      check: { type: "boolean", default: false },
    },
  });
  const simsys =
    values.simsys ?? process.env.SIMSYS ?? path.join(repoRoot, "build/gcc14-release/simsys");
  return {
    simsys: path.resolve(simsys),
    jobs: values.jobs === undefined ? availableParallelism() : positiveWhole("--jobs", values.jobs),
    only: values.only === undefined ? null : values.only.split(","),
    out: path.resolve(values.out ?? defaultOut),
    check: values.check,
  };
}
