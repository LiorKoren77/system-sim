import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import { useContext } from "react";
import { categoryClass } from "./categories";
import type { Copies } from "./copies";
import { FrameContext } from "./frame-context";
import type { NodeData } from "./to-flow-elements";

// One node of the system (each bar's label is short, so a box drawn small still shows it: the busy
// bar's percentage is utilisation, the load bar's rate what the client offers). The band across the top names its category in the category's colour
// (what the box is); the bars show its load at the slider's frame (bars.ts: utilisation, the queue
// building, a client's offered rate against its peak); the border and the pill say its zone (how it
// is doing):
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
        {state?.offeredPerSecond != null ? (
          <BarRow label={rate(state.offeredPerSecond)} fill={state.load} className="load" />
        ) : (
          <BarRow
            label={state?.busy ? `${Math.round(state.busy.mean * 100)}%` : "–"}
            fill={state?.busy?.mean ?? null}
            peak={state?.busy?.peak ?? null}
            ticks={[data.yellow, data.red]}
            className={`busy zone-${zone ?? "none"}`}
            pill={zone}
          />
        )}
        {state?.queue && (
          <BarRow
            label={`${count(state.queue.backlog)} queued`}
            fill={state.queue.fill}
            className="queue"
          />
        )}
      </div>
    </div>
  );
}

const zoneWord = { green: "ok", yellow: "yellow", red: "red" } as const;

// A bar with its label: `fill` from 0 to 1 (past 1 drawn full), the bucket's `peak` as a thin
// mark, and `ticks` (the zones' thresholds) as notches. The fill's width glides over a quarter of
// a second when the frame changes (theme.css): an animation between two real frames, so a jump
// reads as motion; it shows nothing between buckets that the data does not have.
function BarRow(props: {
  label: string;
  fill: number | null;
  className: string;
  peak?: number | null;
  ticks?: readonly (number | null)[];
  pill?: "green" | "yellow" | "red" | null;
}) {
  const percent = (v: number) => `${Math.min(Math.max(v, 0), 1) * 100}%`;
  return (
    <div className={`bar-row ${props.className}`}>
      <div className="bar">
        {props.fill !== null && <div className="fill" style={{ width: percent(props.fill) }} />}
        {props.ticks?.map((t, i) =>
          t === null ? null : <div key={i} className="tick" style={{ left: percent(t) }} />,
        )}
        {props.peak != null && <div className="peak" style={{ left: percent(props.peak) }} />}
      </div>
      <span className="bar-label">{props.label}</span>
      {props.pill != null && (
        <span className={`zone-pill zone-${props.pill}`}>{zoneWord[props.pill]}</span>
      )}
    </div>
  );
}

// 2923830 → "2.92M", 50 → "50".
function count(n: number): string {
  if (n >= 1e9) return `${Number((n / 1e9).toPrecision(3))}G`;
  if (n >= 1e6) return `${Number((n / 1e6).toPrecision(3))}M`;
  if (n >= 1e3) return `${Number((n / 1e3).toPrecision(3))}k`;
  return String(Number(n.toPrecision(3)));
}

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
