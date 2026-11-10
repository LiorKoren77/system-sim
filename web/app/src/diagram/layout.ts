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

export interface Layout {
  readonly nodes: ReadonlyMap<string, Placed>;
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
    ? graph.datacenters.map((dc) => ({
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

function collect(root: ElkNode, grouped: boolean): Layout {
  const nodes = new Map<string, Placed>();
  const groups = new Map<string, Placed>();
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
  return { nodes, groups };
}
