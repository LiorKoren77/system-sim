// Where each node of the diagram goes, computed by ELK (the Eclipse Layout Kernel, compiled to
// JavaScript): its "layered" algorithm draws a directed graph left to right in columns, the way
// the book draws a request's path from clients to storage, with as few crossing edges as it can.
// Clients are put in the first column. With two or more datacenters, each is a box (an ELK compound
// node) holding its nodes, and edges may cross between boxes.
//
// Computed once per graph: nodes keep their places through the day, so the eye follows the
// colours, not motion. ELK is deterministic, so the same graph always gets the same places.
import type { Diagram, GraphLine } from "@simsys/stream";
import type { ELK, ElkExtendedEdge, ElkNode, ElkPort } from "elkjs/lib/elk-api";
import { sidesFor, type Box, type Side } from "./sides";

export const NODE_WIDTH = 176;
export const NODE_HEIGHT = 96;

export interface Placed {
  readonly x: number; // relative to the node's datacenter box when it has one
  readonly y: number;
  readonly width: number;
  readonly height: number;
  readonly parent: string | null; // the box's id, `dc:<name>`
}

export interface Point {
  readonly x: number;
  readonly y: number;
}

export interface Layout {
  readonly nodes: ReadonlyMap<string, Placed>;
  // Each link's route, `link-<i>` in the graph's link order: ELK's orthogonal path from the source
  // box's border to the target's, around the boxes in between, in the diagram's coordinates.
  readonly routes: ReadonlyMap<string, readonly Point[]>;
  // The sides each link joins, `link-<i>`: where its route leaves and enters the boxes.
  readonly sides: ReadonlyMap<string, { readonly source: Side; readonly target: Side }>;
  readonly groups: ReadonlyMap<string, Placed>; // by `dc:<name>`, in the graph's datacenter order
}

// ELK is about 1.5 MB of JavaScript: loaded on first use, as its own file, so the page's first
// paint does not wait for it.
let elk: Promise<ELK> | undefined;
function loadElk(): Promise<ELK> {
  elk ??= import("elkjs/lib/elk.bundled.js").then(({ default: Elk }) => new Elk());
  return elk;
}

const rootOptions = {
  "elk.algorithm": "layered",
  "elk.direction": "RIGHT",
  "elk.hierarchyHandling": "INCLUDE_CHILDREN",
  // ELK routes the edges too: right-angled lines that go around the boxes, the way the book draws a
  // system, with their coordinates relative to the whole diagram (not to a datacenter box).
  "elk.edgeRouting": "ORTHOGONAL",
  "elk.json.edgeCoords": "ROOT",
  "elk.layered.spacing.edgeNodeBetweenLayers": "24",
  "elk.spacing.edgeNode": "16",
  "elk.spacing.edgeEdge": "10",
  "elk.layered.cycleBreaking.strategy": "MODEL_ORDER",
  "elk.layered.considerModelOrder.strategy": "NODES_AND_EDGES",
  "elk.layered.spacing.nodeNodeBetweenLayers": "64",
  "elk.spacing.nodeNode": "28",
  "elk.padding": "[top=16,left=16,bottom=16,right=16]",
};

const groupOptions = { "elk.padding": "[top=32,left=16,bottom=16,right=16]" };

export const groupId = (datacenter: string) => `dc:${datacenter}`;

// A grid cell of a hand-placed diagram: a box and the space to its right and below.
export const CELL_WIDTH = NODE_WIDTH + 72;
export const CELL_HEIGHT = NODE_HEIGHT + 56;

type LinkSides = { source: Side; target: Side };

// The layout of `graph`, following its scenario's diagram.yaml when it has one (`diagram`, from the
// dataset line):
// - with `cells`, every box goes on its cell of a grid, and each link joins the sides the file
//   names or the sides that face each other, drawn as right-angled lines (React Flow's
//   smoothstep); ELK places nothing, since it cannot route around boxes it did not place;
// - otherwise ELK places the boxes, in two passes. The first lays them out with edges attached
//   anywhere; from where the boxes land, each link gets the sides the file names or the pair that
//   face each other (sides.ts). The second pass fixes each link's ends to those sides as ELK ports
//   and routes every edge around the boxes into them. Without ports, a left-to-right layout attaches
//   edges only on the left and right sides.
export async function layoutGraph(graph: GraphLine, diagram?: Diagram): Promise<Layout> {
  if (diagram?.cells) return placeOnCells(graph, diagram.cells, diagram.sides);
  const first = await runElk(graph, null);
  const sides = chooseSides(graph, first, diagram?.sides);
  return { ...(await runElk(graph, sides)), sides };
}

// Each link's sides: the file's for the links it names (`"<from> -> <to>"`), else those that face
// each other in `placed`.
function chooseSides(
  graph: GraphLine,
  placed: Pick<Layout, "nodes" | "groups">,
  named: Diagram["sides"],
): Map<string, LinkSides> {
  const sides = new Map<string, LinkSides>();
  graph.links.forEach((link, i) => {
    const pair = named?.[`${link.from} -> ${link.to}`];
    if (pair) {
      sides.set(`link-${i}`, { source: pair[0], target: pair[1] });
      return;
    }
    const from = absoluteBox(placed, link.from);
    const to = absoluteBox(placed, link.to);
    if (from && to) sides.set(`link-${i}`, sidesFor(from, to));
  });
  return sides;
}

// The boxes on their cells; a datacenter's box around its nodes' cells, the nodes placed relative to
// it (as React Flow places a node inside a parent).
function placeOnCells(
  graph: GraphLine,
  cells: NonNullable<Diagram["cells"]>,
  named: Diagram["sides"],
): Layout {
  const grouped = graph.datacenters.length > 1;
  const absolute = new Map<string, Placed>();
  for (const node of graph.nodes) {
    const [column, row] = cells[node.id] ?? [0, 0];
    absolute.set(node.id, {
      x: column * CELL_WIDTH,
      y: row * CELL_HEIGHT,
      width: NODE_WIDTH,
      height: NODE_HEIGHT,
      parent: null,
    });
  }
  const groups = new Map<string, Placed>();
  const nodes = new Map<string, Placed>();
  if (!grouped) {
    for (const [id, box] of absolute) nodes.set(id, box);
  } else {
    const pad = { side: 16, top: 32 };
    for (const dc of graph.datacenters) {
      const members = graph.nodes.filter((n) => n.datacenter === dc.name);
      const boxes = members.map((n) => absolute.get(n.id)).filter((b) => b !== undefined);
      if (boxes.length === 0) continue;
      const x = Math.min(...boxes.map((b) => b.x)) - pad.side;
      const y = Math.min(...boxes.map((b) => b.y)) - pad.top;
      const right = Math.max(...boxes.map((b) => b.x + b.width)) + pad.side;
      const bottom = Math.max(...boxes.map((b) => b.y + b.height)) + pad.side;
      const id = groupId(dc.name);
      groups.set(id, { x, y, width: right - x, height: bottom - y, parent: null });
      for (const n of members) {
        const b = absolute.get(n.id);
        if (b) nodes.set(n.id, { ...b, x: b.x - x, y: b.y - y, parent: id });
      }
    }
  }
  const placed = { nodes, groups };
  return { ...placed, routes: new Map(), sides: chooseSides(graph, placed, named) };
}

const elkSide: Readonly<Record<Side, string>> = {
  left: "WEST",
  right: "EAST",
  top: "NORTH",
  bottom: "SOUTH",
};

async function runElk(
  graph: GraphLine,
  sides: ReadonlyMap<string, { source: Side; target: Side }> | null,
): Promise<Omit<Layout, "sides">> {
  const grouped = graph.datacenters.length > 1;
  // A port per link end, on its side: `link-3:source` on the box the link leaves.
  const portsOf = (id: string): ElkPort[] =>
    sides === null
      ? []
      : graph.links.flatMap((link, i) => {
          const pair = sides.get(`link-${i}`);
          if (pair === undefined) return [];
          const ends: ElkPort[] = [];
          if (link.from === id) ends.push(port(`link-${i}:source`, pair.source));
          if (link.to === id) ends.push(port(`link-${i}:target`, pair.target));
          return ends;
        });
  const leaf = (node: GraphLine["nodes"][number]): ElkNode => {
    const ports = portsOf(node.id);
    return {
      id: node.id,
      width: NODE_WIDTH,
      height: NODE_HEIGHT,
      ports,
      layoutOptions: {
        ...(node.type === "client" ? { "elk.layered.layering.layerConstraint": "FIRST" } : {}),
        ...(ports.length > 0 ? { "elk.portConstraints": "FIXED_SIDE" } : {}),
      },
    };
  };
  const children: ElkNode[] = grouped
    ? byFlow(graph).map((dc) => ({
        id: groupId(dc.name),
        layoutOptions: groupOptions,
        children: graph.nodes.filter((n) => n.datacenter === dc.name).map(leaf),
      }))
    : graph.nodes.map(leaf);
  const edges: ElkExtendedEdge[] = graph.links.map((link, i) => {
    const id = `link-${i}`;
    const fixed = sides?.has(id) ?? false;
    return {
      id,
      sources: [fixed ? `${id}:source` : link.from],
      targets: [fixed ? `${id}:target` : link.to],
    };
  });
  const root = await (
    await loadElk()
  ).layout({ id: "root", layoutOptions: rootOptions, children, edges });
  return collect(root, grouped);
}

function port(id: string, side: Side): ElkPort {
  // Zero-sized, so a route ends exactly on the box's border.
  return { id, width: 0, height: 0, layoutOptions: { "elk.port.side": elkSide[side] } };
}

// A node's box in the diagram's coordinates: a node inside a datacenter box is placed relative to it.
export function absoluteBox(layout: Pick<Layout, "nodes" | "groups">, id: string): Box | undefined {
  const place = layout.nodes.get(id);
  if (place === undefined) return undefined;
  const parent = place.parent === null ? undefined : layout.groups.get(place.parent);
  return {
    x: place.x + (parent?.x ?? 0),
    y: place.y + (parent?.y ?? 0),
    width: place.width,
    height: place.height,
  };
}

// The datacenters in the order their traffic flows: the one whose links mostly leave it for the
// others first. ELK breaks a cycle by the order the boxes are given (MODEL_ORDER), so a box given
// first goes left; given in the file's order, youtube's eu-west, whose viewers' traffic flows into
// us-east, went right, and each of its links looped back across the whole diagram. Ties keep the
// file's order.
function byFlow(graph: GraphLine): GraphLine["datacenters"] {
  const dcOf = new Map(graph.nodes.map((n) => [n.id, n.datacenter]));
  const net = new Map(graph.datacenters.map((dc) => [dc.name, 0]));
  for (const link of graph.links) {
    const from = dcOf.get(link.from);
    const to = dcOf.get(link.to);
    if (from === undefined || to === undefined || from === to) continue;
    net.set(from, (net.get(from) ?? 0) + 1);
    net.set(to, (net.get(to) ?? 0) - 1);
  }
  // Array.prototype.sort is stable (ES2019): equal flows keep the file's order.
  return [...graph.datacenters].sort((a, b) => (net.get(b.name) ?? 0) - (net.get(a.name) ?? 0));
}

function collect(root: ElkNode, grouped: boolean): Omit<Layout, "sides"> {
  const nodes = new Map<string, Placed>();
  const groups = new Map<string, Placed>();
  const routes = new Map<string, Point[]>();
  for (const edge of root.edges ?? []) {
    const points: Point[] = [];
    for (const section of edge.sections ?? []) {
      if (points.length === 0) points.push(section.startPoint);
      points.push(...(section.bendPoints ?? []), section.endPoint);
    }
    if (points.length >= 2) routes.set(edge.id, points);
  }
  const place = (n: ElkNode, parent: string | null): Placed => ({
    x: n.x ?? 0,
    y: n.y ?? 0,
    width: n.width ?? NODE_WIDTH,
    height: n.height ?? NODE_HEIGHT,
    parent,
  });
  for (const child of root.children ?? []) {
    if (!grouped) {
      nodes.set(child.id, place(child, null));
      continue;
    }
    groups.set(child.id, place(child, null));
    for (const leaf of child.children ?? []) nodes.set(leaf.id, place(leaf, child.id));
  }
  return { nodes, groups, routes };
}
