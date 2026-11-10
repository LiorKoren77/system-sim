import type { DatasetStore, Diagram as DiagramFile, GraphLine } from "@simsys/stream";
import { ReactFlow, type EdgeTypes, type NodeTypes } from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { useEffect, useMemo, useState } from "react";
import { layoutGraph, type Layout } from "./layout";
import { DatacenterBox } from "./DatacenterBox";
import { NodeCard } from "./NodeCard";
import { RoutedEdge } from "./RoutedEdge";
import { peaksOf } from "./bars";
import { busiestFlow, flowDuration, linkFlowsAt } from "./flow";
import { FlowContext, FrameContext } from "./frame-context";
import { nodeStatesAt, pipeZonesAt, toFlowElements, withPipeZones } from "./to-flow-elements";

// A module constant: React Flow warns when the node types object changes between renders.
const nodeTypes: NodeTypes = { system: NodeCard, datacenter: DatacenterBox };
const edgeTypes: EdgeTypes = { routed: RoutedEdge };

// One layout per graph object, kept while the page lives (a graph is never edited in place, and a
// dataset's diagram comes with its graph).
const layouts = new WeakMap<GraphLine, Promise<Layout>>();

function layoutOf(graph: GraphLine, diagram: DiagramFile | undefined): Promise<Layout> {
  let layout = layouts.get(graph);
  if (layout === undefined) {
    layout = layoutGraph(graph, diagram);
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
  playing: boolean; // the dashes move only while the day plays
}) {
  const { store, frame, selected, onSelect, playing } = props;
  const graph = store.graph;
  const diagramFile = store.dataset?.diagram;
  const [layout, setLayout] = useState<{ graph: GraphLine; layout: Layout } | null>(null);
  useEffect(() => {
    if (graph === undefined) return;
    let live = true;
    void layoutOf(graph, diagramFile).then((l) => {
      if (live) setLayout({ graph, layout: l });
    });
    return () => {
      live = false;
    };
  }, [graph, diagramFile]);
  const current = layout !== null && layout.graph === graph ? layout.layout : null;
  // The structure: new objects only when the layout or the selection changes, never per frame.
  const structure = useMemo(
    () => (graph && current ? toFlowElements(graph, current, selected) : null),
    [graph, current, selected],
  );
  // The frame's state. The store mutates as frames arrive; its version marks a change.
  const version = store.version;
  // The bars' scales, over the whole dataset (bars.ts): computed once it has loaded.
  const peaks = useMemo(
    () => (graph ? peaksOf(graph, store) : null),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- version stands for the store's contents
    [graph, store, store.status === "ready" ? version : -1],
  );
  const states = useMemo(
    () => (graph && peaks ? nodeStatesAt(graph, store, frame, peaks) : new Map()),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- version stands for the store's contents
    [graph, store, version, frame, peaks],
  );
  // The links' traffic: dash speeds on a scale up to the busiest link of the whole dataset.
  const busiest = useMemo(
    () => (graph && store.status === "ready" ? busiestFlow(graph, store) : 0),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- version stands for the store's contents
    [graph, store, version],
  );
  const durationsKey = graph
    ? linkFlowsAt(graph, store, frame)
        .map((f) => flowDuration(f, busiest) ?? "")
        .join(",")
    : "";
  const durations = useMemo(
    () => durationsKey.split(",").map((d) => (d === "" ? null : Number(d))),
    [durationsKey],
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
      <FlowContext value={durations}>
        <div className={playing ? "diagram playing" : "diagram"}>
          <ReactFlow
            nodes={structure.nodes}
            edges={edges}
            nodeTypes={nodeTypes}
            edgeTypes={edgeTypes}
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
      </FlowContext>
    </FrameContext>
  );
}
