// The lines of `simsys run --stream` (docs/scenario-schema.md, "Command line: simsys run --stream")
// and the `dataset` line the precompute puts in front of them (phase 5b spec §6), as TypeScript
// types. Each line is one JSON object with a `type`; the union below is discriminated on it, so a
// `switch (message.type)` narrows `message` to that line's type in each case (TypeScript's
// "discriminated union": the C++ analogue is std::variant visited by type).
//
// Only the keys this code reads are typed. The contract lets simsys add keys (rule 2), and a reader
// ignores those it does not know (rule 1), so the summary's report keeps the rest untyped.

// A row of a frame: one number per column of the graph's `*_columns` list, null where the column does
// not apply to the row or the value is not finite.
export type Row = readonly (number | null)[];

export interface RunLine {
  readonly type: "run";
  readonly format_version: number;
  readonly scenario: string;
  readonly seed: number;
  readonly duration_s: number;
  readonly step_s: number;
  readonly warmup_s: number;
  readonly bucket_s: number;
  readonly frames: number;
  readonly days?: number; // `run --days N` (phase 5b); absent without it
}

export interface GraphNode {
  readonly id: string;
  readonly type: string;
  readonly datacenter: string;
  readonly replicas: number;
  readonly shards: number | null;
  readonly capacity_per_s: number | null;
  readonly unit: string | null; // requests, units, MB, replica-requests; null on a client
  readonly yellow: number | null; // the utilisation thresholds of the node's zones
  readonly red: number | null;
  readonly queue_limit: number | null; // the room: requests that may wait before drops begin
  readonly workers?: number;
  readonly connection_cap?: number;
}

export interface GraphLink {
  readonly from: string;
  readonly to: string;
  readonly direction: string;
  readonly ops: readonly string[];
  readonly one_way_s: number;
  readonly bandwidth_mb_s: number | null;
  readonly pipe_row: number | null; // the link's row in a frame's `links`; null without a bandwidth
}

export interface GraphOp {
  readonly client: string;
  readonly op: string;
  readonly share: number;
}

export interface GraphSlo {
  readonly metric: string;
  readonly node: string | null;
  readonly op: string | null;
  readonly max: number;
  readonly over_s: number | null;
}

export interface GraphFault {
  readonly kind: string;
  readonly target: string;
  readonly at_s: number;
  readonly for_s: number | null; // null for an instant cache_flush
  readonly replicas?: number | null; // a node_down's copies taken; null: every copy (phase 5b)
  readonly primary?: boolean; // a node_down of a sql_db's primary (phase 5b)
}

export interface GraphLine {
  readonly type: "graph";
  readonly rev: number;
  readonly scenario: string;
  readonly datacenters: readonly { readonly name: string }[];
  readonly zones: readonly string[];
  readonly nodes: readonly GraphNode[];
  readonly links: readonly GraphLink[];
  readonly ops: readonly GraphOp[];
  readonly slos: readonly GraphSlo[];
  readonly faults: readonly GraphFault[];
  readonly node_columns: readonly string[];
  readonly link_columns: readonly string[];
  readonly op_columns: readonly string[];
  readonly estimated_columns: readonly string[];
}

export interface FrameLine {
  readonly type: "frame";
  readonly rev: number;
  readonly t_s: number; // the bucket's first step, in run time
  readonly dt_s: number; // the bucket's length
  readonly nodes: readonly Row[];
  readonly links: readonly Row[];
  readonly ops: readonly Row[];
}

// What `run --report json` prints; typed where the page reads it.
export interface Report {
  readonly format_version: number;
  readonly scenario: string;
  readonly steps: number;
  readonly slo: readonly SloResult[];
  readonly faults: readonly FaultBlock[];
  readonly [key: string]: unknown;
}

export interface SloResult {
  readonly metric: string;
  readonly node: string | null;
  readonly op: string | null;
  readonly observed: number | null;
  readonly max: number;
  readonly status: string; // pass, fail, skipped
  readonly note: string | null;
  readonly window_start_s?: number;
}

export interface FaultBlock {
  readonly at_s: number;
  readonly kind: string;
  readonly node?: string;
  readonly datacenter?: string;
  readonly for_s: number | null;
  readonly during: Readonly<Record<string, unknown>>;
  readonly after: Readonly<Record<string, unknown>>;
  readonly recovered_at_s?: number | null;
  readonly [key: string]: unknown;
}

export interface SummaryLine {
  readonly type: "summary";
  readonly exit_code: number;
  readonly report: Report;
}

export interface ErrorLine {
  readonly type: "error";
  readonly file: string | null;
  readonly line: number;
  readonly column: number;
  readonly message: string;
}

// The seam check of a day (phase 5b spec §7.2): whether the frame ending at 48:00 equals the one
// ending at 24:00, and the column that differed most.
export interface Seam {
  readonly status: "repeats" | "drifts";
  readonly worst: {
    readonly row: "node" | "link" | "op";
    readonly id: string;
    readonly column: string;
    readonly a: number | null;
    readonly b: number | null;
    readonly rel: number;
  } | null;
}

// A side of a box in the diagram, where a link leaves or enters it.
export type Side = "left" | "right" | "top" | "bottom";

// A scenario's diagram.yaml (phase 5b, beside its scenario.yaml), checked by the precompute against
// the graph: `cells` puts every box on a [column, row] of a grid; `sides` chooses which sides a
// link joins, by `"<from> -> <to>"`. Either may be absent: ELK then places the boxes, or picks the
// sides that face each other.
export interface Diagram {
  readonly cells?: Readonly<Record<string, readonly [number, number]>>;
  readonly sides?: Readonly<Record<string, readonly [Side, Side]>>;
}

export interface DatasetLine {
  readonly type: "dataset";
  readonly dataset_version: number;
  readonly id: string;
  readonly title: string | null;
  readonly chapter: number | null;
  readonly kind: "day" | "run";
  readonly window: { readonly from_s: number; readonly to_s: number };
  readonly loops: boolean;
  readonly seam: Seam | null;
  readonly source: { readonly scenario: string; readonly args: readonly string[] };
  readonly diagram?: Diagram; // the scenario's diagram.yaml, when it has one
}

export type StreamMessage = DatasetLine | RunLine | GraphLine | FrameLine | SummaryLine | ErrorLine;
