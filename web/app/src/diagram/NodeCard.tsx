import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import type { NodeData } from "./to-flow-elements";

// One node of the system: its id, its type and copies, and, for the frame on the slider, its mean
// utilisation and its zone (as a word too: colour is never the only signal). A client shows the
// rate it offers instead, since a source has no queue to fill.
export function NodeCard({ data }: NodeProps<Node<NodeData>>) {
  const copies = data.replicas > 1 ? ` ×${data.replicas}` : "";
  const shards = data.shards !== null ? ` · ${data.shards} shards` : "";
  return (
    <div className="node-card">
      <Handle type="target" position={Position.Left} />
      <div className="node-id">{data.id}</div>
      <div className="node-type">
        {data.type}
        {copies}
        {shards}
      </div>
      <div className="node-state">
        {data.offeredPerSecond !== null
          ? `offers ${rate(data.offeredPerSecond)}`
          : `${data.rhoPercent === null ? "–" : `${data.rhoPercent}%`} · ${data.zone ?? "no data"}`}
      </div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}

// 1500 → "1.5k/s", 2000000 → "2M/s".
export function rate(perSecond: number): string {
  const scaled = (value: number, unit: string) => `${Number(value.toPrecision(3))}${unit}/s`;
  if (perSecond >= 1e9) return scaled(perSecond / 1e9, "G");
  if (perSecond >= 1e6) return scaled(perSecond / 1e6, "M");
  if (perSecond >= 1e3) return scaled(perSecond / 1e3, "k");
  return scaled(perSecond, "");
}
