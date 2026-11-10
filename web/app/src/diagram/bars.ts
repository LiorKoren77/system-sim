// The scales of a box's bars (NodeCard.tsx): what "full" means for each.
//
// - busy: utilisation, drain / capacity, full at 1. Its scale needs nothing from the dataset.
// - queue: the backlog, full at the node's own largest backlog in the dataset. Against the node's
//   room (`queue_limit`) the bar would not move: notification-system's ios_workers queue 2.9M jobs
//   in the 09:00 blast against a room of 7.9 billion. Against its own peak, the bar shows the
//   build-up during a surge and the drain after it, which is the transition the model has (a pool's
//   utilisation saturates at once, its queue does not).
// - load: a client's offered rate, full at its own busiest frame, so a 20x surge fills a bar that
//   sat at a twentieth of it.
import type { DatasetStore, GraphLine } from "@simsys/stream";

export interface Peaks {
  readonly backlog: ReadonlyMap<string, number>; // by node id: the largest backlog of the dataset
  readonly offered: ReadonlyMap<string, number>; // by client id: the most it offered in a frame
}

export function peaksOf(graph: GraphLine, store: DatasetStore): Peaks {
  const backlog = new Map<string, number>();
  const offered = new Map<string, number>();
  graph.nodes.forEach((node, row) => {
    if (node.type === "client") {
      offered.set(node.id, largest(offeredSeries(graph, store, node.id)));
    } else {
      backlog.set(node.id, largest(store.nodeColumn(row, "backlog")));
    }
  });
  return { backlog, offered };
}

// A client's offered rate in each frame: the sum of its ops'.
export function offeredSeries(graph: GraphLine, store: DatasetStore, client: string): Float64Array {
  const sum = new Float64Array(store.frameCount);
  graph.ops.forEach((op, row) => {
    if (op.client !== client) return;
    const column = store.opColumn(row, "offered_per_s");
    for (let k = 0; k < sum.length; ++k) sum[k] = (sum[k] ?? 0) + (column[k] || 0);
  });
  return sum;
}

function largest(values: Float64Array): number {
  let m = 0;
  for (const v of values) if (v > m) m = v;
  return m;
}
