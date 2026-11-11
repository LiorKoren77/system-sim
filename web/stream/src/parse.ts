// One line of the stream read by the output contract (docs/scenario-schema.md, "The output
// contract"): an unknown line type is ignored (rule 1), an unknown key is dropped (rule 1), another
// format_version is refused (rule 3: it means something this code does not know changed), and a
// line that lacks a key its type needs is an error, never a half-read message.
import type { StreamMessage } from "./messages";

export const FORMAT_VERSION = 1;
export const DATASET_VERSION = 1;

export type Parsed =
  | { readonly kind: "message"; readonly message: StreamMessage }
  | { readonly kind: "ignored" }
  | { readonly kind: "error"; readonly message: string };

// The keys each line type needs, in the order the schema lists them; the message keeps only these
// (and the optional ones), so an unknown key never reaches the page.
const required: Readonly<Record<StreamMessage["type"], readonly string[]>> = {
  dataset: ["dataset_version", "id", "kind", "window", "loops"],
  run: ["format_version", "scenario", "duration_s", "step_s", "warmup_s", "bucket_s", "frames"],
  graph: ["rev", "nodes", "links", "ops", "node_columns", "link_columns", "op_columns"],
  frame: ["rev", "t_s", "dt_s", "nodes", "links", "ops"],
  summary: ["exit_code", "report"],
  error: ["message"],
};

const optional: Readonly<Record<StreamMessage["type"], readonly string[]>> = {
  dataset: ["title", "chapter", "seam", "source", "diagram"],
  run: ["seed", "days"],
  graph: ["scenario", "datacenters", "zones", "slos", "faults", "estimated_columns"],
  frame: [],
  summary: [],
  error: ["file", "line", "column"],
};

function isKnownType(type: unknown): type is StreamMessage["type"] {
  return typeof type === "string" && Object.hasOwn(required, type);
}

function versionRefusal(key: string, expected: number, found: unknown): Parsed {
  return {
    kind: "error",
    message: `this page reads ${key} ${expected}; the data says ${String(found)}: rebuild the datasets (npm run precompute)`,
  };
}

// `lineNumber` counts from 1, for the messages.
export function parseLine(text: string, lineNumber: number): Parsed {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return { kind: "error", message: `line ${lineNumber} is not JSON` };
  }
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return { kind: "error", message: `line ${lineNumber} is not a JSON object` };
  }
  const object = value as Record<string, unknown>;
  const type = object.type;
  if (!isKnownType(type)) return { kind: "ignored" };
  // The version first: a line of another version may have other keys, and its message is the one
  // that says what to do.
  if (type === "run" && object.format_version !== FORMAT_VERSION) {
    return versionRefusal("format_version", FORMAT_VERSION, object.format_version);
  }
  if (type === "dataset" && object.dataset_version !== DATASET_VERSION) {
    return versionRefusal("dataset_version", DATASET_VERSION, object.dataset_version);
  }
  const kept: Record<string, unknown> = { type };
  for (const key of required[type]) {
    if (!Object.hasOwn(object, key)) {
      return { kind: "error", message: `line ${lineNumber}: a ${type} needs ${key}` };
    }
    kept[key] = object[key];
  }
  for (const key of optional[type]) {
    if (Object.hasOwn(object, key)) kept[key] = object[key];
  }
  // The checks above are what the contract promises; the shapes inside (rows of numbers) are
  // simsys's to keep, and its tests do.
  return { kind: "message", message: kept as unknown as StreamMessage };
}
