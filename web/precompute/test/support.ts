// Graphs and frames built in code for the precompute's rules (the seam, the window).
import type { FrameLine, GraphLine } from "@simsys/stream";

const nodeColumns = [
  "demand_per_s",
  "drain_per_s",
  "dropped_per_s",
  "rejected_per_s",
  "backlog",
  "peak_backlog",
  "mean_rho",
  "peak_rho",
  "zone",
  "red_s",
  "latency_ms",
  "queue_lag_s",
  "connections",
  "dead_lettered_per_s",
  "deduplicated_per_s",
];

// A client and one service `q`, one op, no bandwidth link.
export const graph: GraphLine = {
  type: "graph",
  rev: 0,
  scenario: "test",
  datacenters: [{ name: "main" }],
  zones: ["green", "yellow", "red"],
  nodes: [
    {
      id: "users",
      type: "client",
      datacenter: "main",
      replicas: 1,
      shards: null,
      capacity_per_s: null,
      unit: null,
      yellow: null,
      red: null,
      queue_limit: null,
    },
    {
      id: "q",
      type: "service",
      datacenter: "main",
      replicas: 1,
      shards: null,
      capacity_per_s: 1000,
      unit: "requests",
      yellow: 0.7,
      red: 0.9,
      queue_limit: 10000,
    },
  ],
  links: [
    {
      from: "users",
      to: "q",
      direction: "inout",
      ops: [],
      one_way_s: 0.00025,
      bandwidth_mb_s: null,
      pipe_row: null,
    },
  ],
  ops: [{ client: "users", op: "read", share: 1 }],
  slos: [],
  faults: [],
  node_columns: nodeColumns,
  link_columns: [],
  op_columns: [
    "offered_per_s",
    "attempts_per_s",
    "answered_per_s",
    "timed_out_per_s",
    "mean_ms",
    "p99_ms",
  ],
  estimated_columns: ["p99_ms"],
};

// A frame at `t_s` whose service has `backlog` (and peak 5000 at most, the scale of the absolute
// tolerance), the rest steady.
export function frame(t_s: number, backlog: number | null, peakBacklog = 5000): FrameLine {
  return {
    type: "frame",
    rev: 0,
    t_s,
    dt_s: 120,
    nodes: [
      Array<null>(15).fill(null),
      [500, 500, 0, 0, backlog, peakBacklog, 0.5, 0.6, 0, 0, 2, null, null, null, null],
    ],
    links: [],
    ops: [[500, 500, 500, 0, 2.5, 6]],
  };
}

// The frames of a `--days 2` run in 2 min buckets: 1440, each at its t_s, the service's backlog
// `backlogAt(k)` in frame k.
export function twoDays(backlogAt: (k: number) => number | null = () => 0): FrameLine[] {
  return Array.from({ length: 1440 }, (_, k) => frame(k * 120, backlogAt(k)));
}
