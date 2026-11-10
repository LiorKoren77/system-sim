import type { DatasetStore, GraphLine } from "@simsys/stream";
import { ReactFlow, type NodeTypes } from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { useEffect, useMemo, useState } from "react";
import { layoutGraph, type Layout } from "./layout";
import { NodeCard } from "./NodeCard";
import { FrameContext } from "./frame-context";
import { nodeStatesAt, pipeZonesAt, toFlowElements, withPipeZones } from "./to-flow-elements";

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
  // The structure: new objects only when the layout or the selection changes, never per frame.
  const structure = useMemo(
    () => (graph && current ? toFlowElements(graph, current, selected) : null),
    [graph, current, selected],
  );
  // The frame's state. The store mutates as frames arrive; its version marks a change.
  const version = store.version;
  const states = useMemo(
    () => (graph ? nodeStatesAt(graph, store, frame) : new Map()),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- version stands for the store's contents
    [graph, store, version, frame],
  );
  // The pipes' zones as one string, so the edges are rebuilt only when one of them changes.
  const zonesKey = graph ? pipeZonesAt(graph, store, frame).join(",") : "";
  const edges = useMemo(
    () =>
      structure && graph
        ? withPipeZones(
            structure.edges,
            zonesKey.split(",").map((z) => (z === "" ? null : (z as "green" | "yellow" | "red"))),
          )
        : [],
    [structure, graph, zonesKey],
  );
  if (structure === null) return <div className="diagram" />;
  return (
    <FrameContext value={states}>
      <div className="diagram">
        <ReactFlow
          nodes={structure.nodes}
          edges={edges}
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
    </FrameContext>
  );
}
