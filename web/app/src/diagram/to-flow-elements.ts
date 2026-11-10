// The diagram in two parts, both pure functions tested without a browser:
//
// - the structure (toFlowElements): React Flow's nodes and edges, from the graph, its layout and
//   the selection. It does not depend on the frame, so while the day plays React Flow is handed
//   the same objects and leaves its boxes alone. A new node object makes React Flow measure and
//   place that node again; handing it new ones twelve times a second made the boxes flicker
//   under the mouse.
// - the state of one frame (nodeStatesAt, pipeZonesAt): what each node card shows, read by the
//   cards from a React context (FrameContext.ts), and the zone of each bandwidth link, applied to
//   the edges only when it changes (withPipeZones).
import type { DatasetStore, GraphLine } from "@simsys/stream";
import { MarkerType, type Edge, type EdgeMarker, type Node } from "@xyflow/react";
import { categoryOf, type Category } from "./categories";
import { peaksOf, type Peaks } from "./bars";
import { copiesAt, type Copies } from "./copies";
import { absoluteBox, type Layout } from "./layout";
import { sidesFor, type Side } from "./sides";

export type Zone = "green" | "yellow" | "red";

// What a node card shows that no frame changes. The index signature is React Flow's requirement
// for a node's data.
export interface NodeData {
  readonly [key: string]: unknown;
  readonly id: string;
  readonly type: string;
  readonly replicas: number;
  readonly shards: number | null;
  readonly category: Category;
  readonly yellow: number | null; // the utilisation thresholds of its zones, ticks on its busy bar
  readonly red: number | null;
}

// What a node card shows for one frame.
export interface NodeState {
  // The zone of the bucket: the worst step's, judged against the node's yellow and red utilisation
  // thresholds (5a §6.2). null on a client, which has no queue.
  readonly zone: Zone | null;
  readonly rhoPercent: number | null; // the bucket's mean utilisation (drain / capacity)
  readonly offeredPerSecond: number | null; // a client's: the sum of its ops' offered rates
  readonly copies: Copies | null; // how many of its copies are up (copies.ts); null on a client
  // The bars (bars.ts): utilisation (mean and the bucket's peak); the backlog against the node's own
  // largest; a client's offered rate against its own busiest frame. null where it does not apply.
  readonly busy: { readonly mean: number; readonly peak: number } | null;
  readonly queue: { readonly fill: number; readonly backlog: number } | null;
  readonly load: number | null;
}

export function toFlowElements(
  graph: GraphLine,
  layout: Layout,
  selected: string | null,
): { nodes: Node[]; edges: Edge[] } {
  // React Flow needs a box before the nodes inside it.
  const groups: Node[] = [...layout.groups].map(([id, box]) => ({
    id,
    type: "datacenter",
    position: { x: box.x, y: box.y },
    style: { width: box.width, height: box.height },
    data: { label: id.slice("dc:".length) },
    className: "datacenter",
    selectable: false,
    draggable: false,
  }));
  const nodes: Node[] = graph.nodes.map((node) => {
    const place = layout.nodes.get(node.id);
    const data: NodeData = {
      id: node.id,
      type: node.type,
      replicas: node.replicas,
      shards: node.shards,
      category: categoryOf(node.type),
      yellow: node.yellow,
      red: node.red,
    };
    return {
      id: node.id,
      type: "system",
      position: { x: place?.x ?? 0, y: place?.y ?? 0 },
      ...(place?.parent ? { parentId: place.parent, extent: "parent" as const } : {}),
      data,
      ...(node.type === "client" ? { className: "client" } : {}),
      selected: node.id === selected,
      draggable: false,
    };
  });
  const edges: Edge[] = graph.links.map((link, i) => {
    const sides = layout.sides.get(`link-${i}`) ?? fallbackSides(layout, link.from, link.to);
    const id = `link-${i}`;
    const route = layout.routes.get(id);
    return {
      id,
      source: link.from,
      target: link.to,
      sourceHandle: `source-${sides.source}`,
      targetHandle: `target-${sides.target}`,
      // ELK's route around the boxes when it gave one; otherwise right-angled lines between the
      // facing sides (sides.ts). `index` is the link's place in the graph, for its traffic.
      type: "routed",
      data: route ? { index: i, points: route } : { index: i },
      // The arrowhead points at the box the link sends requests to.
      markerEnd: arrow(null),
      ...(link.pipe_row === null ? {} : { className: "pipe" }),
    };
  });
  return { nodes: [...groups, ...nodes], edges };
}

// Each node's state at `frame`, by id.
export function nodeStatesAt(
  graph: GraphLine,
  store: DatasetStore,
  frame: number,
  peaks: Peaks = peaksOf(graph, store),
): Map<string, NodeState> {
  const states = new Map<string, NodeState>();
  const copies = copiesAt(graph, store.times()[frame] ?? 0, store.durations()[frame] ?? 0);
  graph.nodes.forEach((node, row) => {
    const isClient = node.type === "client";
    const value = (name: string) => at(store.nodeColumn(row, name), frame);
    const mean = value("mean_rho");
    const peakBacklog = peaks.backlog.get(node.id) ?? 0;
    const peakOffered = peaks.offered.get(node.id) ?? 0;
    const offered = isClient ? offeredAt(graph, store, node.id, frame) : null;
    states.set(node.id, {
      zone: isClient ? null : zoneName(graph, at(store.nodeColumn(row, "zone"), frame)),
      rhoPercent: isClient ? null : rhoAt(store, row, frame),
      offeredPerSecond: offered,
      copies: copies.get(node.id) ?? null,
      busy: isClient || Number.isNaN(mean) ? null : { mean, peak: value("peak_rho") || mean },
      queue:
        isClient || !(peakBacklog > 0)
          ? null
          : { fill: (value("backlog") || 0) / peakBacklog, backlog: value("backlog") || 0 },
      load: offered !== null && peakOffered > 0 ? offered / peakOffered : null,
    });
  });
  return states;
}

// The zone of each link at `frame`, in the graph's link order: null for a link without a bandwidth
// (it has no queue of its own) and without a store.
export function pipeZonesAt(
  graph: GraphLine,
  store: DatasetStore | null,
  frame: number,
): (Zone | null)[] {
  return graph.links.map((link) =>
    link.pipe_row === null || store === null
      ? null
      : zoneName(graph, at(store.linkColumn(link.pipe_row, "zone"), frame)),
  );
}

// The edges with each pipe's zone as a class; an edge whose class does not change is the same
// object, so React Flow redraws nothing for it.
export function withPipeZones(edges: readonly Edge[], zones: readonly (Zone | null)[]): Edge[] {
  return edges.map((edge, i) => {
    const zone = zones[i];
    if (zone === null || zone === undefined) return edge;
    return { ...edge, className: `pipe zone-${zone}`, markerEnd: arrow(zone) };
  });
}

function at(column: Float64Array, frame: number): number {
  return column[frame] ?? NaN;
}

function zoneName(graph: GraphLine, value: number): Zone | null {
  if (Number.isNaN(value)) return null;
  const name = graph.zones[value];
  return name === "green" || name === "yellow" || name === "red" ? name : null;
}

function rhoAt(store: DatasetStore, row: number, frame: number): number | null {
  const rho = at(store.nodeColumn(row, "mean_rho"), frame);
  return Number.isNaN(rho) ? null : Math.round(rho * 100);
}

function offeredAt(graph: GraphLine, store: DatasetStore, client: string, frame: number): number {
  let sum = 0;
  graph.ops.forEach((op, row) => {
    if (op.client === client) sum += at(store.opColumn(row, "offered_per_s"), frame) || 0;
  });
  return sum;
}

// The facing sides from the boxes' places, for a link the layout gave no sides.
function fallbackSides(layout: Layout, from: string, to: string): { source: Side; target: Side } {
  const a = absoluteBox(layout, from);
  const b = absoluteBox(layout, to);
  return a && b ? sidesFor(a, b) : { source: "right", target: "left" };
}

// An arrowhead for a link: the lines' grey, or a pipe's zone colour. React Flow draws a marker with
// a fill colour, not a CSS variable, so the colours are given here (theme.css has the same ones).
const arrowColours: Readonly<Record<Zone | "none", string>> = {
  none: "#8a8f96",
  green: "#4cc27e",
  yellow: "#f0b429",
  red: "#ff6b5e",
};

function arrow(zone: Zone | null): EdgeMarker {
  return {
    type: MarkerType.ArrowClosed,
    width: 18,
    height: 18,
    color: arrowColours[zone ?? "none"],
  };
}
