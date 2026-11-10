// The traffic on the diagram's links, drawn as dashes moving from a link's source to its target
// (RoutedEdge.tsx). A link carries what its target box serves in the frame, its drain (requests per
// second; for a bandwidth link, the megabytes per second through its own pipe). That is the
// rate that actually gets through: when a box goes down, its drain falls to 0 and the dashes into
// it stop, so an outage shows as traffic stopping, not only as a red box. A box fed by several
// links serves them together, and the frame does not split its drain by link, so each of those
// links shows the box's whole drain: the dashes say how busy the destination is.
import type { DatasetStore, GraphLine } from "@simsys/stream";

// Each link's traffic at `frame`, in the graph's link order.
export function linkFlowsAt(graph: GraphLine, store: DatasetStore, frame: number): number[] {
  const rowOf = new Map(graph.nodes.map((n, row) => [n.id, row]));
  return graph.links.map((link) => {
    const column =
      link.pipe_row !== null
        ? store.linkColumn(link.pipe_row, "drain_per_s")
        : store.nodeColumn(rowOf.get(link.to) ?? 0, "drain_per_s");
    const value = column[frame] ?? NaN;
    return Number.isNaN(value) ? 0 : value;
  });
}

// The most traffic any link carries in any frame of the dataset: the top of the speed scale.
export function busiestFlow(graph: GraphLine, store: DatasetStore): number {
  let busiest = 0;
  for (let frame = 0; frame < store.frameCount; ++frame) {
    for (const flow of linkFlowsAt(graph, store, frame)) busiest = Math.max(busiest, flow);
  }
  return busiest;
}

// How long one dash takes to move one dash cycle along a link carrying `flow` when the busiest
// carries `busiest`: 0.5 s for the busiest, up to 2.5 s for the quietest, on a logarithmic scale,
// because rates span orders of magnitude (50/s to 40k/s in the labs) and a linear one would leave
// the quiet links still. Rounded to a tenth of a second: changing the duration restarts the
// dashes' animation, so it should change only when the rate does visibly. null: nothing flows.
export function flowDuration(flow: number, busiest: number): number | null {
  if (!(flow > 0) || !(busiest > 0)) return null;
  const share = Math.min(1, Math.log1p(flow) / Math.log1p(busiest));
  return Math.round((2.5 - 2 * share) * 10) / 10;
}
