import type { DatasetStore, GraphLine } from "@simsys/stream";
import { ReactFlow, type NodeTypes } from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { useEffect, useMemo, useState } from "react";
import { layoutGraph, type Layout } from "./layout";
import { NodeCard } from "./NodeCard";
import { toFlowElements } from "./to-flow-elements";

// A module constant: React Flow warns when the node types object changes between renders.
const nodeTypes: NodeTypes = { system: NodeCard };

// One layout per graph object, kept while the page lives (a graph is never edited in place).
const layouts = new WeakMap<GraphLine, Promise<Layout>>();

function layoutOf(graph: GraphLine): Promise<Layout> {
  let layout = layouts.get(graph);
  if (layout === undefined) {
    layout = layoutGraph(graph);
    layouts.set(graph, layout);
  }
  return layout;
}

// The system's diagram at the slider's frame, drawn by React Flow (pan and zoom; nodes stay where
// the layout put them). Clicking a node selects it for the charts; clicking the background clears
// the selection.
export function Diagram(props: {
  store: DatasetStore;
  frame: number;
  selected: string | null;
  onSelect: (id: string | null) => void;
}) {
  const { store, frame, selected, onSelect } = props;
  const graph = store.graph;
  const [layout, setLayout] = useState<{ graph: GraphLine; layout: Layout } | null>(null);
  useEffect(() => {
    if (graph === undefined) return;
    let live = true;
    void layoutOf(graph).then((l) => {
      if (live) setLayout({ graph, layout: l });
    });
    return () => {
      live = false;
    };
  }, [graph]);
  const current = layout !== null && layout.graph === graph ? layout.layout : null;
  // store.version changes when frames arrive; the elements depend on it through the store.
  const elements = useMemo(
    () => (graph && current ? toFlowElements(graph, current, store, frame, selected) : null),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- the store mutates; version marks it
    [graph, current, store, store.version, frame, selected],
  );
  if (elements === null) return <div className="diagram" />;
  return (
    <div className="diagram">
      <ReactFlow
        nodes={elements.nodes}
        edges={elements.edges}
        nodeTypes={nodeTypes}
        nodesDraggable={false}
        nodesConnectable={false}
        elementsSelectable
        fitView
        minZoom={0.2}
        onNodeClick={(_, node) => {
          if (node.type === "system") onSelect(node.id);
        }}
        onPaneClick={() => {
          onSelect(null);
        }}
      />
    </div>
  );
}
