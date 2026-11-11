// The zone strip's grid: a row per node, a cell per frame, the zone the node's box shows at that
// frame (0 green, 1 yellow, 2 red: the worst step of the bucket against the node's thresholds,
// 5a §6.2, and for a queue also its lag and its room, boxZone). The whole window's health at a
// glance; ZoneStrip.tsx paints it on a canvas and maps clicks back with the two functions below.
import type { DatasetStore, GraphLine } from "@simsys/stream";
import { peaksOf } from "../diagram/bars";
import { boxZone, nodeStatesAt } from "../diagram/to-flow-elements";

const ZONE_INDEX = { green: 0, yellow: 1, red: 2 } as const;

export const NO_ZONE = 255; // a frame where the node had no zone (null in the stream)

export interface ZoneGrid {
  readonly rows: readonly { readonly id: string; readonly row: number }[];
  readonly frames: number;
  readonly cells: Uint8Array; // rows × frames, row-major
}

// A client has no zone (a source has no capacity to fill), so it has no row.
export function zoneGrid(graph: GraphLine, store: DatasetStore): ZoneGrid {
  const rows = graph.nodes
    .map((node, row) => ({ id: node.id, row, type: node.type }))
    .filter((n) => n.type !== "client")
    .map(({ id, row }) => ({ id, row }));
  const frames = store.frameCount;
  const cells = new Uint8Array(rows.length * frames).fill(NO_ZONE);
  const peaks = peaksOf(graph, store);
  for (let k = 0; k < frames; ++k) {
    const states = nodeStatesAt(graph, store, k, peaks);
    rows.forEach(({ id }, r) => {
      const zone = boxZone(states.get(id));
      if (zone !== null) cells[r * frames + k] = ZONE_INDEX[zone];
    });
  }
  return { rows, frames, cells };
}

// The frame under x on a strip `width` wide, kept inside the window.
export function frameAtX(x: number, width: number, frames: number): number {
  return Math.min(Math.max(Math.floor((x / width) * frames), 0), frames - 1);
}

// The row under y, or null past the last.
export function rowAtY(y: number, rowHeight: number, rows: number): number | null {
  const r = Math.floor(y / rowHeight);
  return r >= 0 && r < rows ? r : null;
}
