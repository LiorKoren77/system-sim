// Where each node of the diagram goes, computed by ELK (the Eclipse Layout Kernel, compiled to
// JavaScript): its "layered" algorithm draws a directed graph left to right in columns, the way
// the book draws a request's path from clients to storage, with as few crossing edges as it can.
// Clients are put in the first column. With two or more datacenters, each is a box (an ELK compound
// node) holding its nodes, and edges may cross between boxes.
//
// Computed once per graph: nodes keep their places through the day, so the eye follows the
// colours, not motion. ELK is deterministic, so the same graph always gets the same places.
import type { GraphLine } from "@simsys/stream";
import type { ELK, ElkExtendedEdge, ElkNode } from "elkjs/lib/elk-api";

export const NODE_WIDTH = 176;
export const NODE_HEIGHT = 68;

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

export async function layoutGraph(graph: GraphLine): Promise<Layout> {
  const grouped = graph.datacenters.length > 1;
  const leaf = (node: GraphLine["nodes"][number]): ElkNode => ({
    id: node.id,
    width: NODE_WIDTH,
    height: NODE_HEIGHT,
    ...(node.type === "client"
      ? { layoutOptions: { "elk.layered.layering.layerConstraint": "FIRST" } }
      : {}),
  });
  const children: ElkNode[] = grouped
    ? byFlow(graph).map((dc) => ({
        id: groupId(dc.name),
        layoutOptions: groupOptions,
        children: graph.nodes.filter((n) => n.datacenter === dc.name).map(leaf),
      }))
    : graph.nodes.map(leaf);
  const edges: ElkExtendedEdge[] = graph.links.map((link, i) => ({
    id: `link-${i}`,
    sources: [link.from],
    targets: [link.to],
  }));
  const root = await (
    await loadElk()
  ).layout({ id: "root", layoutOptions: rootOptions, children, edges });
  return collect(root, grouped);
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

function collect(root: ElkNode, grouped: boolean): Layout {
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
