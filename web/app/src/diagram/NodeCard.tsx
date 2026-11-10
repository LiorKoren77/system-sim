import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import { useContext } from "react";
import { categoryClass } from "./categories";
import type { Copies } from "./copies";
import { FrameContext } from "./frame-context";
import type { NodeData } from "./to-flow-elements";

// One node of the system. The band across the top names its category in the category's colour
// (what the box is); the border and the pill say its zone at the slider's frame (how it is doing):
// a plain border and a quiet "ok" when green, amber when yellow, a thick red border and a red fill
// when red, so trouble stands out and a calm system is not a wall of green. The zone is written
// as a word too: colour is never the only signal. A client shows the rate it offers instead,
// since a source has no queue to fill.
export function NodeCard({ data }: NodeProps<Node<NodeData>>) {
  const state = useContext(FrameContext).get(data.id);
  const zone = state?.zone ?? null;
  const copies = data.replicas > 1 ? ` ×${data.replicas}` : "";
  const shards = data.shards !== null ? ` · ${data.shards} shards` : "";
  // A node with copies is drawn as a stack of cards (theme.css), its slots the copies up.
  const stacked = data.replicas > 1 ? " stacked" : "";
  return (
    <div className={`node-card ${categoryClass(data.category)} zone-${zone ?? "none"}${stacked}`}>
      <Handles />
      <div className="node-category">{data.category}</div>
      <div className="node-body">
        <div className="node-id">{data.id}</div>
        <div className="node-type">
          {data.type}
          {copies}
          {shards}
        </div>
        {state?.copies && <CopySlots copies={state.copies} />}
        <div className="node-state">
          {state?.offeredPerSecond != null ? (
            <span>offers {rate(state.offeredPerSecond)}</span>
          ) : (
            <>
              <span>{state?.rhoPercent == null ? "–" : `${state.rhoPercent}% busy`}</span>
              {zone !== null && <span className={`zone-pill zone-${zone}`}>{zoneWord[zone]}</span>}
            </>
          )}
        </div>
      </div>
    </div>
  );
}

const zoneWord = { green: "ok", yellow: "yellow", red: "red" } as const;

// One slot per copy, filled while it is up; the slots narrow as the copies grow, so a stack of 20
// or 100 still fits the box. Shown for a node with copies, and for a single copy only while it is
// down. simsys knows how many copies are down, not which (copies.ts), so the slots up are always
// the first ones.
function CopySlots({ copies }: { copies: Copies }) {
  if (copies.of === 1 && copies.up === 1) return null;
  const label = copies.primaryDown
    ? `primary down, ${copies.up} of ${copies.of} up`
    : `${copies.up} of ${copies.of} up`;
  return (
    <div className="copies" title={label} aria-label={label}>
      <span className="slots">
        {Array.from({ length: copies.of }, (_, i) => (
          <span key={i} className={i < copies.up ? "slot up" : "slot down"} />
        ))}
      </span>
      {copies.up < copies.of && <span className="copies-label">{label}</span>}
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
