// The SLO table's rows, from the summary's report (the observed window: for a day, day 2). An
// SLO's `max` takes its metric's kind (schema, "SLO"): a duration for the latencies, the queue
// lag and the staleness; a share for the rates and the utilisation. So is what was measured.
import type { DatasetLine, GraphLine, Report } from "@simsys/stream";
import { clockLabel, durationLabel } from "../timeline/clock";
import { latency, percent } from "../format";

export interface SloRow {
  readonly metric: string;
  readonly target: string;
  readonly bound: string;
  readonly observed: string;
  readonly status: string; // pass, fail, skipped
  readonly note: string | null;
  readonly window: string | null; // a windowed SLO's worst window
}

const durations = new Set(["p50_latency", "p95_latency", "p99_latency", "queue_lag", "staleness"]);

export function sloRows(report: Report, graph: GraphLine, dataset: DatasetLine): SloRow[] {
  return report.slo.map((slo, i) => {
    const value = (v: number) => (durations.has(slo.metric) ? latency(v) : percent(v));
    const over = graph.slos[i]?.over_s ?? null;
    const start = slo.window_start_s;
    return {
      metric: slo.metric.replaceAll("_", " "),
      target: target(slo.node, slo.op),
      bound: `≤ ${value(slo.max)}`,
      observed: slo.observed === null ? "—" : value(slo.observed),
      status: slo.status,
      note: slo.note,
      window:
        over === null || start === undefined || start === null
          ? null
          : `worst ${durationLabel(over)} from ${clockLabel(start, dataset)}`,
    };
  });
}

// Without a node, an SLO judges the worst node (drop rate, reject rate) or the worst queue.
function target(node: string | null, op: string | null): string {
  if (node === null) return "every node";
  return op === null ? node : `${node} · ${op}`;
}
