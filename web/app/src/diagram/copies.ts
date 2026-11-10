// How many copies of each node are up in a frame, from the scripted faults in the graph. This is
// what simsys models: a node with replicas is one queue whose capacity is scaled by the copies up,
// the survivors sharing the load evenly (docs/scenario-schema.md, "Faults"). It knows how many
// copies are down, not which ones, so the diagram shows a count, never a particular server.
import type { GraphLine } from "@simsys/stream";

export interface Copies {
  readonly up: number;
  readonly of: number;
  readonly primaryDown: boolean; // a sql_db whose primary is down: writes wait, reads go on
}

// The copies of every node (not the clients, which keep sending) in the bucket [t_s, t_s + dt_s).
// A fault on in any part of the bucket counts, as the bucket's zone is its worst step's.
export function copiesAt(graph: GraphLine, t_s: number, dt_s: number): Map<string, Copies> {
  const down = new Map<string, number>();
  const primary = new Set<string>();
  const on = (at: number, length: number | null) =>
    length !== null && at < t_s + dt_s && at + length > t_s;
  for (const fault of graph.faults) {
    if (!on(fault.at_s, fault.for_s)) continue;
    if (fault.kind === "dc_down") {
      for (const node of graph.nodes) {
        if (node.datacenter === fault.target && node.type !== "client") {
          down.set(node.id, node.replicas);
        }
      }
    } else if (fault.kind === "node_down") {
      const node = graph.nodes.find((n) => n.id === fault.target);
      if (node === undefined) continue;
      // A primary is one copy; a count is that many; neither is every copy.
      const taken = fault.primary === true ? 1 : (fault.replicas ?? node.replicas);
      if (fault.primary === true) primary.add(node.id);
      down.set(node.id, (down.get(node.id) ?? 0) + taken);
    }
  }
  const copies = new Map<string, Copies>();
  for (const node of graph.nodes) {
    if (node.type === "client") continue;
    const of = node.replicas;
    copies.set(node.id, {
      up: Math.max(0, of - Math.min(of, down.get(node.id) ?? 0)),
      of,
      primaryDown: primary.has(node.id),
    });
  }
  return copies;
}
