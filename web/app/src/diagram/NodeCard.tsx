import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import { useContext } from "react";
import { FrameContext } from "./frame-context";
import type { NodeData } from "./to-flow-elements";

// One node of the system: its id, its type and copies, and, for the frame on the slider, its mean
// utilisation and its zone (as a word too: colour is never the only signal). A client shows the
// rate it offers instead, since a source has no queue to fill.
export function NodeCard({ data }: NodeProps<Node<NodeData>>) {
  const state = useContext(FrameContext).get(data.id);
  const zone = state?.zone ?? null;
  const copies = data.replicas > 1 ? ` ×${data.replicas}` : "";
  const shards = data.shards !== null ? ` · ${data.shards} shards` : "";
  return (
    <div className={`node-card zone-${zone ?? "none"}`}>
      <div className="node-id">{data.id}</div>
      <div className="node-type">
        {data.type}
        {copies}
        {shards}
      </div>
      <div className="node-state">
        {state?.offeredPerSecond != null
          ? `offers ${rate(state.offeredPerSecond)}`
          : `${state?.rhoPercent == null ? "–" : `${state.rhoPercent}%`} · ${zone ?? "no data"}`}
      </div>
      <Handles />
    </div>
  );
}

// A connection point on each side, for edges arriving (target) and leaving (source): an edge picks
// the pair of sides that face the other box (sides.ts). The ids are what an edge names.
const sides = [
  ["left", Position.Left],
  ["right", Position.Right],
  ["top", Position.Top],
  ["bottom", Position.Bottom],
] as const;

function Handles() {
  return sides.flatMap(([side, position]) => [
    <Handle key={`t-${side}`} id={`target-${side}`} type="target" position={position} />,
    <Handle key={`s-${side}`} id={`source-${side}`} type="source" position={position} />,
  ]);
}

// 1500 → "1.5k/s", 2000000 → "2M/s".
export function rate(perSecond: number): string {
  const scaled = (value: number, unit: string) => `${Number(value.toPrecision(3))}${unit}/s`;
  if (perSecond >= 1e9) return scaled(perSecond / 1e9, "G");
  if (perSecond >= 1e6) return scaled(perSecond / 1e6, "M");
  if (perSecond >= 1e3) return scaled(perSecond / 1e3, "k");
  return scaled(perSecond, "");
}
