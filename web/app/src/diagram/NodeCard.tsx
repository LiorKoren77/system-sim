import { Handle, Position, type Node, type NodeProps } from "@xyflow/react";
import { useContext } from "react";
import { categoryClass } from "./categories";
import type { Copies } from "./copies";
import { FrameContext } from "./frame-context";
import { count, duration, rate } from "../format";
import { boxZone, type NodeData, type Queue } from "./to-flow-elements";

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
  const zone = boxZone(state);
  // A queue's lag gets a row of its own, which also carries the zone pill (it has the room); with
  // several consumer groups the lag goes to the tooltip, so the box does not overflow.
  const lag =
    state !== undefined &&
    state.lagSeconds !== null &&
    state.lagSeconds >= 1 &&
    state.queues.length <= 1
      ? state.lagSeconds
      : null;
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
            label={busyLabel(data.type, state?.busy?.mean ?? null)}
            title={busyTitle(data.type, state?.busy?.mean ?? null)}
            fill={state?.busy?.mean ?? null}
            peak={state?.busy?.peak ?? null}
            ticks={[data.yellow, data.red]}
            className={`busy zone-${zone ?? "none"}`}
            pill={lag !== null ? null : zone}
          />
        )}
        {state?.queues.map((q) => (
          <BarRow
            key={q.consumer ?? "own"}
            label={queueLabel(q, state.queues.length)}
            title={`${count(q.backlog)} ${q.consumer === null ? "waiting" : `messages waiting for ${q.consumer}`}${
              state.lagSeconds != null && state.lagSeconds >= 1
                ? `, lag ${duration(state.lagSeconds)}`
                : ""
            }`}
            fill={q.fill}
            className={`queue zone-${q.zone ?? "none"}`}
          />
        ))}
        {lag !== null && (
          <div className={`lag-row lag-${state?.lagZone ?? "none"}`}>
            <span>lag {duration(lag)}</span>
            {zone !== null && <span className={`zone-pill zone-${zone}`}>{zoneWord[zone]}</span>}
          </div>
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
  title?: string;
  fill: number | null;
  className: string;
  peak?: number | null;
  ticks?: readonly (number | null)[];
  pill?: "green" | "yellow" | "red" | null;
}) {
  const percent = (v: number) => `${Math.min(Math.max(v, 0), 1) * 100}%`;
  return (
    <div className={`bar-row ${props.className}`} {...(props.title ? { title: props.title } : {})}>
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

// A queue's busy bar is its broker's (accepting and handing out messages, about a million a second),
// not how full it is: its waiting messages and its lag are the rows below it.
function busyLabel(type: string, mean: number | null): string {
  if (mean === null) return "–";
  const percent = `${Math.round(mean * 100)}%`;
  return type === "message_queue" ? `broker ${percent}` : percent;
}

function busyTitle(type: string, mean: number | null): string {
  if (mean === null) return "";
  const percent = `${Math.round(mean * 100)}%`;
  return type === "message_queue" ? `broker ${percent} busy` : `${percent} busy`;
}

// "2.92M waiting"; with several consumer groups, each row names its pool instead.
function queueLabel(q: Queue, rows: number): string {
  if (rows > 1 && q.consumer !== null) return `${count(q.backlog)} · ${q.consumer}`;
  return `${count(q.backlog)} waiting`;
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
