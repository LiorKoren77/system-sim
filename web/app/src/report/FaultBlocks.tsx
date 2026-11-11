import type { DatasetStore, FaultBlock } from "@simsys/stream";
import { count, duration, rate } from "../format";
import { clockLabel, durationLabel } from "../timeline/clock";
import { faultsInWindow, frameAt } from "./window-faults";

interface Shown {
  readonly kind: "day" | "run";
  readonly window: { readonly from_s: number; readonly to_s: number };
}

function shownOf(store: DatasetStore): Shown {
  return {
    kind: store.dataset?.kind ?? "run",
    window: store.dataset?.window ?? { from_s: 0, to_s: store.run?.duration_s ?? 0 },
  };
}

const num = (v: unknown): number | null => (typeof v === "number" ? v : null);

// The report's block for each scripted fault in the window, as `simsys run` prints it: during the
// fault (measured against a fault-free twin of the run: what was offered and served, what was lost,
// the queue's peak against its room, the SLOs it broke) and after it (when the backlog drained, a
// cache was warm again, the node was back). A click moves the slider to the fault.
export function FaultBlocks(props: { store: DatasetStore; onSeek: (frame: number) => void }) {
  const { store, onSeek } = props;
  const shown = shownOf(store);
  const faults = faultsInWindow(store.summary?.report.faults ?? [], shown.window);
  if (faults.length === 0) return <p className="hint">No scripted fault in this window.</p>;
  return (
    <ul className="faults">
      {faults.map((fault) => (
        <li key={`${fault.kind}-${fault.at_s}`}>
          <button
            type="button"
            className="fault"
            onClick={() => {
              onSeek(frameAt(store.times(), fault.at_s));
            }}
          >
            <span className="what">
              {fault.kind} {fault.node ?? fault.datacenter ?? ""}
            </span>
            <span className="when">
              at {clockLabel(fault.at_s, shown)}
              {fault.for_s !== null && ` · for ${durationLabel(fault.for_s)}`}
            </span>
            <Facts title="during" facts={during(fault)} />
            <Facts title="after" facts={after(fault, shown)} />
          </button>
        </li>
      ))}
    </ul>
  );
}

function Facts({ title, facts }: { title: string; facts: [string, string][] }) {
  if (facts.length === 0) return null;
  return (
    <span className="facts">
      <span className="facts-title">{title}</span>
      {facts.map(([name, value]) => (
        <span key={name} className="fact">
          <span className="name">{name}</span>
          <span className="value">{value}</span>
        </span>
      ))}
    </span>
  );
}

function during(fault: FaultBlock): [string, string][] {
  const d = fault.during;
  const facts: [string, string][] = [];
  const offered = num(d.offered_per_s);
  const served = num(d.served_per_s);
  const lost = num(d.lost);
  const forGood = num(d.lost_requests);
  const peak = num(d.backlog_peak);
  const room = num(d.room);
  const unit = typeof d.lost_unit === "string" ? d.lost_unit : "requests";
  if (offered !== null) facts.push(["offered", rate(offered)]);
  if (served !== null) facts.push(["served", rate(served)]);
  if (lost !== null) facts.push(["lost", `${count(lost)} ${unit}`]);
  if (forGood !== null) facts.push(["lost for good", `${count(forGood)} requests`]);
  if (peak !== null) {
    facts.push(["peak backlog", room === null ? count(peak) : `${count(peak)} of ${count(room)}`]);
  }
  const broken = Array.isArray(d.over_max) ? (d.over_max as { slo: string; for_s: number }[]) : [];
  for (const b of broken) facts.push([`broke ${b.slo}`, `for ${duration(b.for_s)}`]);
  return facts;
}

function after(fault: FaultBlock, shown: Shown): [string, string][] {
  const a = fault.after;
  const facts: [string, string][] = [];
  const recovered = num(fault.recovered_at_s);
  const drained = num(a.backlog_drained_in_s);
  const warm = num(a.cache_warm_in_s);
  const retries = num(a.retries_added_attempts);
  const storm = num(a.reconnect_storm_connections);
  const stormOver = num(a.reconnect_storm_over_s);
  if (recovered !== null) {
    facts.push([
      "from → recovered",
      `${clockLabel(fault.at_s, shown)} → ${clockLabel(recovered, shown)}`,
    ]);
  } else if (fault.for_s !== null) {
    facts.push(["recovered", "not in the window"]);
  }
  if (drained !== null) facts.push(["backlog drained in", duration(drained)]);
  if (warm !== null) facts.push(["cache warm in", duration(warm)]);
  if (retries !== null) facts.push(["retries added", `${count(retries)} attempts`]);
  if (storm !== null) {
    facts.push([
      "reconnect storm",
      `${count(storm)} connections${stormOver === null ? "" : ` over ${duration(stormOver)}`}`,
    ]);
  }
  return facts;
}
