// The zone of a queue's waiting messages. Not how full the bar is (that is against the queue's own
// peak of the day, bars.ts, so every queue would be red at its worst moment, harmful or not), but
// the two ways a queue gets into trouble:
// - its lag breaks the scenario's promise: a `queue_lag` SLO bounds how long a message may wait;
// - it runs out of room: the waiting room is the retention × the pool's rate (the pool's
//   queue_limit), and past it the oldest messages are deleted unread (the pool's drops).
// Each is yellow from 70% and red from the bound (the lag) or 90% (the room), the thresholds a
// node's utilisation uses by default (spec §3.3); only a lag SLO that is met makes a queue green. The worse of the two wins, as a node's zone is
// the worst of its bucket. A queue neither judges has no zone: it is holding messages until the
// workers reach them, which is its job, however long that takes.
import type { Zone } from "./to-flow-elements";

const yellowAt = 0.7;
const redAtRoom = 0.9;

export function queueZone(q: {
  readonly lag: number | null; // seconds; null when unknown or unbounded
  readonly lagMax: number | null; // the queue's queue_lag SLO, seconds; null without one
  readonly backlog: number;
  readonly room: number | null;
}): Zone | null {
  const zones: Zone[] = [];
  if (q.lag !== null && q.lagMax !== null && q.lagMax > 0) {
    const share = q.lag / q.lagMax;
    zones.push(share > 1 ? "red" : share >= yellowAt ? "yellow" : "green");
  }
  // The room only warns: a queue far from it is not thereby keeping a promise, so it adds no green
  // (a queue judged by nothing else stays uncoloured).
  if (q.room !== null && q.room > 0) {
    const share = q.backlog / q.room;
    if (share >= redAtRoom) zones.push("red");
    else if (share >= yellowAt) zones.push("yellow");
  }
  return worstZone(zones);
}

// The lag bound the scenario sets on queue `id` (a queue_lag SLO naming it), in seconds.
export function lagSloOf(
  slos: readonly { metric: string; node: string | null; max: number }[],
  id: string,
) {
  return slos.find((s) => s.metric === "queue_lag" && s.node === id)?.max ?? null;
}

// The worst of some zones (null: no zone), as a node's zone is the worst of its bucket's steps.
export function worstZone(zones: readonly (Zone | null)[]): Zone | null {
  if (zones.includes("red")) return "red";
  if (zones.includes("yellow")) return "yellow";
  return zones.includes("green") ? "green" : null;
}
