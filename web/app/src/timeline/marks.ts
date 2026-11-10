// The marks on the time slider: what happens in the window the page plays, so the eye knows where
// to look before it happens. Two kinds, drawn apart (theme.css): a failure (a scripted fault: a
// node or datacenter down, capacity cut, a latency spike, a cache flush) in red, and a surge of load
// (a client's rate spike, or a traffic_spike fault, which is the same thing scripted as a fault) in
// violet. A surge breaks nothing: what turns red then is overload.
import type { GraphLine } from "@simsys/stream";

export interface Mark {
  readonly kind: "failure" | "surge";
  readonly at_s: number;
  readonly for_s: number; // 0 for an instant (a cache flush)
  readonly label: string;
}

export function timelineMarks(
  graph: GraphLine,
  window: { readonly from_s: number; readonly to_s: number },
): Mark[] {
  const marks: Mark[] = [];
  for (const fault of graph.faults) {
    const surge = fault.kind === "traffic_spike";
    marks.push({
      kind: surge ? "surge" : "failure",
      at_s: fault.at_s,
      for_s: fault.for_s ?? 0,
      label: `${fault.kind} ${fault.target}${copiesNote(graph, fault)}`,
    });
  }
  for (const node of graph.nodes) {
    for (const spike of node.spikes ?? []) {
      marks.push({
        kind: "surge",
        at_s: spike.at_s,
        for_s: spike.for_s,
        label: `×${spike.factor} traffic from ${node.id}`,
      });
    }
  }
  return marks
    .filter((m) => m.at_s >= window.from_s && m.at_s < window.to_s)
    .sort((a, b) => a.at_s - b.at_s);
}

// " (1 of 4)" for a node_down that takes some of a node's copies, " (primary)" for a sql_db's.
function copiesNote(graph: GraphLine, fault: GraphLine["faults"][number]): string {
  if (fault.kind !== "node_down") return "";
  if (fault.primary === true) return " (primary)";
  const node = graph.nodes.find((n) => n.id === fault.target);
  if (fault.replicas == null || node === undefined) return "";
  return ` (${fault.replicas} of ${node.replicas})`;
}
