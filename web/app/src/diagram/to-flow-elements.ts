// The diagram's elements for one frame: a pure function of the graph, its layout and the store, so
// it is tested without a browser (React Flow draws what it returns).
import type { DatasetStore, GraphLine } from "@simsys/stream";
import type { Edge, Node } from "@xyflow/react";
import type { Layout } from "./layout";

export type Zone = "green" | "yellow" | "red";

// What a node card shows. The index signature is React Flow's requirement for a node's data.
export interface NodeData {
  readonly [key: string]: unknown;
  readonly id: string;
  readonly type: string;
  readonly replicas: number;
  readonly shards: number | null;
  // The zone of the bucket: the worst step's, judged against the node's yellow and red utilisation
  // thresholds (5a §6.2). null on a client, which has no queue.
  readonly zone: Zone | null;
  readonly rhoPercent: number | null; // the bucket's mean utilisation (drain / capacity)
  readonly offeredPerSecond: number | null; // a client's: the sum of its ops' offered rates
}

export function toFlowElements(
  graph: GraphLine,
  layout: Layout,
  store: DatasetStore | null,
  frame: number,
  selected: string | null,
): { nodes: Node[]; edges: Edge[] } {
  // React Flow needs a box before the nodes inside it.
  const groups: Node[] = [...layout.groups].map(([id, box]) => ({
    id,
    type: "group",
    position: { x: box.x, y: box.y },
    style: { width: box.width, height: box.height },
    data: { label: id.slice("dc:".length) },
    className: "datacenter",
    selectable: false,
    draggable: false,
  }));
  const nodes: Node[] = graph.nodes.map((node, row) => {
    const place = layout.nodes.get(node.id);
    const isClient = node.type === "client";
    const zone = isClient ? null : zoneAt(graph, store, row, frame);
    const data: NodeData = {
      id: node.id,
      type: node.type,
      replicas: node.replicas,
      shards: node.shards,
      zone,
      rhoPercent: isClient ? null : rhoAt(store, row, frame),
      offeredPerSecond: isClient ? offeredAt(graph, store, node.id, frame) : null,
    };
    return {
      id: node.id,
      type: "system",
      position: { x: place?.x ?? 0, y: place?.y ?? 0 },
      ...(place?.parent ? { parentId: place.parent, extent: "parent" as const } : {}),
      data,
      className: isClient ? "client" : `zone-${zone ?? "none"}`,
      selected: node.id === selected,
      draggable: false,
    };
  });
  const edges: Edge[] = graph.links.map((link, i) => {
    const pipe = link.pipe_row;
    const zone = pipe === null ? null : pipeZoneAt(graph, store, pipe, frame);
    return {
      id: `link-${i}`,
      source: link.from,
      target: link.to,
      ...(pipe === null ? {} : { className: `pipe zone-${zone ?? "none"}` }),
    };
  });
  return { nodes: [...groups, ...nodes], edges };
}

function at(column: Float64Array | undefined, frame: number): number {
  return column?.[frame] ?? NaN;
}

function zoneName(graph: GraphLine, value: number): Zone | null {
  if (Number.isNaN(value)) return null;
  const name = graph.zones[value];
  return name === "green" || name === "yellow" || name === "red" ? name : null;
}

function zoneAt(graph: GraphLine, store: DatasetStore | null, row: number, frame: number) {
  return zoneName(graph, at(store?.nodeColumn(row, "zone"), frame));
}

function pipeZoneAt(graph: GraphLine, store: DatasetStore | null, pipe: number, frame: number) {
  return zoneName(graph, at(store?.linkColumn(pipe, "zone"), frame));
}

function rhoAt(store: DatasetStore | null, row: number, frame: number): number | null {
  const rho = at(store?.nodeColumn(row, "mean_rho"), frame);
  return Number.isNaN(rho) ? null : Math.round(rho * 100);
}

function offeredAt(
  graph: GraphLine,
  store: DatasetStore | null,
  client: string,
  frame: number,
): number | null {
  if (store === null) return null;
  let sum = 0;
  graph.ops.forEach((op, row) => {
    if (op.client === client) sum += at(store.opColumn(row, "offered_per_s"), frame) || 0;
  });
  return sum;
}
