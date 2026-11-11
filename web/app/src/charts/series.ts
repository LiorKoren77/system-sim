// What a chart draws, as plain data: uPlot's inputs, built from the store's columns without a
// browser, so they are tested on their own (Chart.tsx only hands them to uPlot).
//
// Every series is a column of the stream, one value per frame, and a frame is a bucket of steps
// (5a §6.2). That decides how each is read:
// - a rate (demand, served, dropped, ...) is the bucket's mean: what came in over the bucket is
//   rate × dt, which is why the queue's conservation law, in = out + Δbacklog, holds per bucket;
// - the backlog is a level: what waits at the bucket's end; peak_backlog the most that waited
//   during it;
// - mean_rho is the bucket's mean utilisation (served / capacity), peak_rho its worst step, so a
//   spike shorter than a bucket still shows in the peak;
// - p99 is estimated: a lognormal fitted to the bucket's mean and variance of the path's latency
//   (the graph lists it in estimated_columns), not a percentile of sampled requests.
import type { DatasetStore, GraphLine, GraphNode } from "@simsys/stream";
import { queueOfPool } from "../diagram/pull";
import { percent } from "../format";

// data: a measured column; yellow, red: a node's zone threshold; limit: another bound (the room).
export type SeriesRole = "data" | "yellow" | "red" | "limit";

export interface SeriesSpec {
  readonly label: string;
  readonly values: Float64Array;
  readonly role: SeriesRole;
}

// One chart: one y axis, in one unit (two measures of different scale get two charts).
export interface ChartSpec {
  readonly key: string;
  readonly title: string;
  readonly unit: string; // "share" is drawn as a percentage
  readonly series: readonly SeriesSpec[];
  readonly about: string; // what the series model, for the reader (shown as the chart's tooltip)
}

// The x of every chart: the frames' start on the window's clock, so a day's chart runs 0–24 h
// whichever day of the run the window is (a looping day is the run's second).
export function xValues(store: DatasetStore, from_s: number): Float64Array {
  return store.times().map((t) => t - from_s);
}

// A rate's unit: what the node counts, per second. MB for a byte-sized node (a CDN, a blob store),
// units for a costed one (a database charges a read 1 unit and a write more), requests otherwise.
export function rateUnit(node: GraphNode): string {
  return `${node.unit ?? "requests"}/s`;
}

// The charts of what is selected: a node's id, or a bandwidth link's "from->to"; none for anything
// else (a link without a bandwidth has no columns of its own).
export function chartsFor(graph: GraphLine, store: DatasetStore, selection: string): ChartSpec[] {
  const row = graph.nodes.findIndex((n) => n.id === selection);
  const node = graph.nodes[row];
  if (node !== undefined) {
    return node.type === "client"
      ? clientCharts(graph, store, node.id)
      : nodeCharts(graph, store, node, row);
  }
  const link = graph.links.find((l) => `${l.from}->${l.to}` === selection);
  if (link?.pipe_row == null) return [];
  return linkCharts(store, link.pipe_row);
}

function constant(length: number, value: number): Float64Array {
  return new Float64Array(length).fill(value);
}

function hasValues(values: Float64Array): boolean {
  return values.some((v) => !Number.isNaN(v));
}

// What each pool pulling from queue `id` has not read yet: simsys counts it as the pool's backlog,
// and the diagram draws it in the queue's box (pull.ts), so the queue's chart shows it too.
function unreadBy(graph: GraphLine, store: DatasetStore, id: string): SeriesSpec[] {
  const series: SeriesSpec[] = [];
  for (const [pool, queue] of queueOfPool(graph)) {
    const row = graph.nodes.findIndex((n) => n.id === pool);
    if (queue === id && row >= 0) {
      series.push({
        label: `unread by ${pool}`,
        values: store.nodeColumn(row, "backlog"),
        role: "data",
      });
    }
  }
  return series;
}

function nodeCharts(
  graph: GraphLine,
  store: DatasetStore,
  node: GraphNode,
  row: number,
): ChartSpec[] {
  const column = (name: string) => store.nodeColumn(row, name);
  const frames = store.frameCount;
  const thresholds: SeriesSpec[] = [];
  if (node.yellow !== null) {
    thresholds.push({
      label: `yellow ${percent(node.yellow)}`,
      values: constant(frames, node.yellow),
      role: "yellow",
    });
  }
  if (node.red !== null) {
    thresholds.push({
      label: `red ${percent(node.red)}`,
      values: constant(frames, node.red),
      role: "red",
    });
  }
  const room: SeriesSpec[] =
    node.queue_limit === null
      ? []
      : [
          {
            label: `room ${node.queue_limit}`,
            values: constant(frames, node.queue_limit),
            role: "limit",
          },
        ];
  const unit = node.unit ?? "requests";
  const charts: ChartSpec[] = [
    {
      key: "utilisation",
      title: "busy",
      unit: "share",
      series: [
        { label: "mean", values: column("mean_rho"), role: "data" },
        { label: "peak", values: column("peak_rho"), role: "data" },
        ...thresholds,
      ],
      about:
        "Utilisation ρ = served / capacity: the bucket's mean, and its worst step (peak). The zone turns yellow and red where the peak crosses the thresholds.",
    },
    {
      key: "backlog",
      title: "waiting",
      unit,
      series: [
        { label: "waiting", values: column("backlog"), role: "data" },
        { label: "peak", values: column("peak_backlog"), role: "data" },
        ...unreadBy(graph, store, node.id),
        ...room,
      ],
      about:
        "The queue in front of the node: a level, what waits at the bucket's end, and the most that waited during it. Past the room, arrivals are dropped. A message queue also shows what each pool pulling from it has not read yet.",
    },
    {
      key: "rates",
      title: "rates",
      unit: rateUnit(node),
      series: [
        { label: "demand", values: column("demand_per_s"), role: "data" },
        { label: "served", values: column("drain_per_s"), role: "data" },
        { label: "dropped", values: column("dropped_per_s"), role: "data" },
        { label: "rejected", values: column("rejected_per_s"), role: "data" },
      ],
      about:
        "Bucket means. What came in (demand × dt) is what was served or dropped plus the change of the backlog: the fluid queue's conservation law, per bucket.",
    },
    {
      key: "latency",
      title: "latency",
      unit: "ms",
      series: [{ label: "latency", values: column("latency_ms"), role: "data" }],
      about:
        "The time a request spends at this node: service time plus the wait in its queue (Little's law: waiting / served).",
    },
  ];
  for (const own of ownCharts(unit)) {
    const values = column(own.column);
    if (hasValues(values)) {
      charts.push({
        key: own.column,
        title: own.title,
        unit: own.unit,
        series: [{ label: own.title, values, role: "data" }],
        about: own.about,
      });
    }
  }
  return charts;
}

// The columns only some types fill (null elsewhere): a chart appears when the node has one.
function ownCharts(unit: string) {
  return [
    {
      column: "connections",
      title: "open connections",
      unit: "connections",
      about: "A WebSocket gateway's open connections: each holds memory and a slot of its cap.",
    },
    {
      column: "queue_lag_s",
      title: "queue lag",
      unit: "s",
      about:
        "How long a message published now waits for a worker: the backlog / the rate the slowest consumer group reads (Little's law).",
    },
    {
      column: "dead_lettered_per_s",
      title: "dead letters",
      unit: `${unit}/s`,
      about: "Jobs given up on once their retries ran out, per second (a bucket mean).",
    },
    {
      column: "deduplicated_per_s",
      title: "deduplicated",
      unit: `${unit}/s`,
      about: "Repeated deliveries the node recognised and dropped, per second (a bucket mean).",
    },
  ] as const;
}

function clientCharts(graph: GraphLine, store: DatasetStore, client: string): ChartSpec[] {
  const estimated = new Set(graph.estimated_columns);
  const p99 = estimated.has("p99_ms") ? "p99 (estimated)" : "p99";
  return graph.ops.flatMap((op, row): ChartSpec[] => {
    if (op.client !== client) return [];
    const column = (name: string) => store.opColumn(row, name);
    return [
      {
        key: `op-${op.op}-rates`,
        title: `${op.op}: requests`,
        unit: "requests/s",
        series: [
          { label: "offered", values: column("offered_per_s"), role: "data" },
          { label: "answered", values: column("answered_per_s"), role: "data" },
          { label: "timed out", values: column("timed_out_per_s"), role: "data" },
        ],
        about:
          "What the client sends of this op, what came back answered, and what timed out: bucket means.",
      },
      {
        key: `op-${op.op}-latency`,
        title: `${op.op}: latency`,
        unit: "ms",
        series: [
          { label: "mean", values: column("mean_ms"), role: "data" },
          { label: p99, values: column("p99_ms"), role: "data" },
        ],
        about:
          "End to end along the op's path. The p99 is estimated: a lognormal fitted to the path's mean and variance, not a percentile of sampled requests.",
      },
    ];
  });
}

function linkCharts(store: DatasetStore, pipe: number): ChartSpec[] {
  const column = (name: string) => store.linkColumn(pipe, name);
  return [
    {
      key: "utilisation",
      title: "busy",
      unit: "share",
      series: [
        { label: "mean", values: column("mean_rho"), role: "data" },
        { label: "peak", values: column("peak_rho"), role: "data" },
      ],
      about: "The share of the link's bandwidth in use: the bucket's mean and its worst step.",
    },
    {
      key: "backlog",
      title: "waiting",
      unit: "MB",
      series: [
        { label: "waiting", values: column("backlog"), role: "data" },
        { label: "peak", values: column("peak_backlog"), role: "data" },
      ],
      about: "Megabytes queued for the link: a level at the bucket's end, and its most.",
    },
    {
      key: "rates",
      title: "rates",
      unit: "MB/s",
      series: [
        { label: "demand", values: column("demand_per_s"), role: "data" },
        { label: "carried", values: column("drain_per_s"), role: "data" },
        { label: "dropped", values: column("dropped_per_s"), role: "data" },
      ],
      about: "Megabytes a second, bucket means: offered to the link, carried, and dropped.",
    },
    {
      key: "latency",
      title: "latency",
      unit: "ms",
      series: [{ label: "latency", values: column("latency_ms"), role: "data" }],
      about: "The link's one-way time plus the wait for bandwidth.",
    },
  ];
}

// uPlot's data: the x column, then each series as numbers with null for NaN (uPlot draws a gap at
// null; a NaN would poison its range).
export function toAligned(
  x: Float64Array,
  series: readonly { readonly values: Float64Array }[],
): [Float64Array, ...(number | null)[][]] {
  return [x, ...series.map((s) => Array.from(s.values, (v) => (Number.isNaN(v) ? null : v)))];
}
