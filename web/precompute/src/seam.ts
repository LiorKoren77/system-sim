// The seam check of a day lived twice (phase 5b spec §7): does the frame ending at 48:00 hold the
// same numbers as the one ending at 24:00?
//
// Why one seam is enough: the engine is deterministic (the state at the start of a step and that
// step's input decide the next state, spec §3.6), and with `run --days` the input repeats every day.
// So if the state at 48:00 equals the state at 24:00, the third day replays the second, and so does
// every later day: the second day is the periodic steady state, and looping it is faithful. A frame
// is not the whole engine state (it shows backlogs, levels and rates, not, say, a cache's warm-up
// counter) and keeps six significant digits, so this is a necessary condition on what a viewer can
// see, which is what a loop's seam would show.
import type { FrameLine, GraphLine, Row, Seam } from "@simsys/stream";

// |a − b| ≤ relative · max(|a|, |b|) + absolute · M, where M is the largest |value| the row's column
// takes over the second day. The relative term absorbs the frames' six-digit rounding; the absolute
// one keeps a crumb of backlog against none on a node that queues thousands from counting as drift.
const relative = 1e-3;
const absolute = 1e-6;

type RowKind = "node" | "link" | "op";

interface Candidate {
  readonly row: RowKind;
  readonly id: string;
  readonly column: string;
  readonly a: number | null;
  readonly b: number | null;
  readonly excess: number; // |a − b| over the tolerance: above 1 is a drift
}

export function seamCheck(
  graph: GraphLine,
  a: FrameLine,
  b: FrameLine,
  day2: readonly FrameLine[],
): Seam {
  const pipeLinks = graph.links.filter((link) => link.pipe_row !== null);
  const kinds: {
    kind: RowKind;
    ids: string[];
    columns: readonly string[];
    rows: (f: FrameLine) => readonly Row[];
  }[] = [
    {
      kind: "node",
      ids: graph.nodes.map((n) => n.id),
      columns: graph.node_columns,
      rows: (f) => f.nodes,
    },
    {
      kind: "link",
      ids: pipeLinks.map((l) => `${l.from}->${l.to}`),
      columns: graph.link_columns,
      rows: (f) => f.links,
    },
    {
      kind: "op",
      ids: graph.ops.map((o) => `${o.client}.${o.op}`),
      columns: graph.op_columns,
      rows: (f) => f.ops,
    },
  ];
  let worst: Candidate | undefined;
  for (const { kind, ids, columns, rows } of kinds) {
    ids.forEach((id, r) => {
      columns.forEach((column, c) => {
        const x = rows(a)[r]?.[c] ?? null;
        const y = rows(b)[r]?.[c] ?? null;
        const excess = excessOf(x, y, largest(day2, rows, r, c));
        if (excess > (worst?.excess ?? 0)) worst = { row: kind, id, column, a: x, b: y, excess };
      });
    });
  }
  if (worst === undefined) return { status: "repeats", worst: null };
  return {
    status: worst.excess > 1 ? "drifts" : "repeats",
    worst: {
      row: worst.row,
      id: worst.id,
      column: worst.column,
      a: worst.a,
      b: worst.b,
      rel: relativeDifference(worst.a, worst.b),
    },
  };
}

// How far apart two values are against their tolerance (0: equal; above 1: a drift).
function excessOf(a: number | null, b: number | null, scale: number): number {
  if (a === null || b === null) return a === b ? 0 : Infinity;
  const difference = Math.abs(a - b);
  if (difference === 0) return 0;
  const tolerance = relative * Math.max(Math.abs(a), Math.abs(b)) + absolute * scale;
  return tolerance === 0 ? Infinity : difference / tolerance;
}

function relativeDifference(a: number | null, b: number | null): number {
  if (a === null || b === null) return 1;
  const larger = Math.max(Math.abs(a), Math.abs(b));
  return larger === 0 ? 0 : Math.abs(a - b) / larger;
}

function largest(
  frames: readonly FrameLine[],
  rows: (f: FrameLine) => readonly Row[],
  r: number,
  c: number,
): number {
  let m = 0;
  for (const frame of frames) m = Math.max(m, Math.abs(rows(frame)[r]?.[c] ?? 0));
  return m;
}
