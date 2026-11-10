// The dataset the page shows, as it arrives: the stream's lines read by the contract (parse.ts)
// into the header lines and the frames as columns. Every view reads this store and nothing else
// (phase 5b spec §9.1).
//
// Frames as columns: per row (a node, a bandwidth link, a client op) and per column, one
// Float64Array over the frames, NaN where the stream wrote null. A chart draws a column directly,
// and the diagram reads column[frame] at the slider's position, so nothing is copied per frame
// shown. (A column-oriented layout: what a time-series database keeps, for the same reason.)
import type {
  DatasetLine,
  ErrorLine,
  FrameLine,
  GraphLine,
  Row,
  RunLine,
  StreamMessage,
  SummaryLine,
} from "./messages";
import { parseLine } from "./parse";
import type { DatasetSource } from "./source";

// loading: lines are still coming; ready: the summary came; error: an error line, a broken line or
// a failed download; truncated: the stream ended without a summary or an error, so the run was
// killed or the download cut short (5a's rule); aborted: the page stopped reading.
export type StoreStatus = "loading" | "ready" | "error" | "truncated" | "aborted";

export interface Problem {
  readonly message: string;
  readonly file?: string | null;
  readonly line?: number;
  readonly column?: number;
}

// The columns of one kind of row (nodes, links or ops): a column per name, a row per entity.
class Columns {
  private data: Float64Array[][] = [];

  constructor(
    private readonly names: readonly string[],
    rows: number,
    capacity: number,
  ) {
    this.data = Array.from({ length: rows }, () =>
      names.map(() => new Float64Array(capacity).fill(NaN)),
    );
  }

  set(frame: number, rows: readonly Row[]): void {
    rows.forEach((row, r) => {
      const columns = this.data[r];
      if (columns === undefined) return; // a row the graph does not have: ignored (rule 1)
      row.forEach((value, c) => {
        const column = columns[c];
        if (column !== undefined) column[frame] = value ?? NaN;
      });
    });
  }

  grow(capacity: number): void {
    this.data = this.data.map((columns) =>
      columns.map((old) => {
        const grown = new Float64Array(capacity).fill(NaN);
        grown.set(old);
        return grown;
      }),
    );
  }

  column(row: number, name: string, length: number): Float64Array {
    const c = this.names.indexOf(name);
    const column = this.data[row]?.[c];
    if (column === undefined) throw new RangeError(`no column ${name} for row ${row}`);
    return column.subarray(0, length);
  }
}

export interface StoreOptions {
  // Subscribers are told after every `batchFrames` frames and at each change of status, not after
  // each frame: a day is 720 frames, and redrawing the page 720 times while it loads is wasted work.
  readonly batchFrames?: number;
}

// Pattern: Observer — views subscribe and are told when the dataset changed; React reads it through
// useSyncExternalStore with `version` as the snapshot (app/src/use-dataset.ts).
export class DatasetStore {
  status: StoreStatus = "loading";
  problem: Problem | undefined;
  dataset: DatasetLine | undefined;
  run: RunLine | undefined;
  graph: GraphLine | undefined; // the latest graph; frames name the revision they follow
  summary: SummaryLine | undefined;
  frameCount = 0;
  version = 0; // bumped at each notification

  private readonly graphs = new Map<number, GraphLine>();
  private readonly listeners = new Set<() => void>();
  private readonly batchFrames: number;
  private sinceNotified = 0;
  private capacity = 0;
  private t = new Float64Array();
  private dt = new Float64Array();
  private nodes: Columns | undefined;
  private links: Columns | undefined;
  private ops: Columns | undefined;

  constructor(options: StoreOptions = {}) {
    this.batchFrames = options.batchFrames ?? 64;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  // Reads `source` to its end (or to `signal`'s abort), applying each line. Never throws: a failure
  // of the source becomes the `error` status with its message.
  async consume(source: DatasetSource, signal?: AbortSignal): Promise<void> {
    let lineNumber = 0;
    let unparsedLast: string | undefined; // a line that failed to parse, held until we know it was the last
    try {
      for await (const line of source.lines(signal)) {
        if (signal?.aborted) break;
        if (unparsedLast !== undefined) {
          // A broken line followed by more lines: not a killed run's partial last line.
          this.fail({ message: unparsedLast });
          return;
        }
        ++lineNumber;
        const parsed = parseLine(line, lineNumber);
        if (parsed.kind === "ignored") continue;
        if (parsed.kind === "error") {
          if (parsed.message.startsWith(`line ${lineNumber} is not JSON`)) {
            unparsedLast = parsed.message;
            continue;
          }
          this.fail({ message: parsed.message });
          return;
        }
        if (!this.apply(parsed.message)) return;
      }
    } catch (error) {
      if (signal?.aborted) {
        this.finish("aborted");
        return;
      }
      this.fail({ message: error instanceof Error ? error.message : String(error) });
      return;
    }
    if (signal?.aborted) this.finish("aborted");
    else if (this.status === "loading") this.finish(this.summary ? "ready" : "truncated");
  }

  times(): Float64Array {
    return this.t.subarray(0, this.frameCount);
  }

  durations(): Float64Array {
    return this.dt.subarray(0, this.frameCount);
  }

  // A node's column over the frames (`row` is the node's index in the graph's `nodes`).
  nodeColumn(row: number, name: string): Float64Array {
    return this.require(this.nodes).column(row, name, this.frameCount);
  }

  // A bandwidth link's column (`row` is the link's `pipe_row`).
  linkColumn(row: number, name: string): Float64Array {
    return this.require(this.links).column(row, name, this.frameCount);
  }

  // A client op's column (`row` is the op's index in the graph's `ops`).
  opColumn(row: number, name: string): Float64Array {
    return this.require(this.ops).column(row, name, this.frameCount);
  }

  // Returns false when the message ended the stream.
  private apply(message: StreamMessage): boolean {
    switch (message.type) {
      case "dataset":
        this.dataset = message;
        return true;
      case "run":
        this.run = message;
        return true;
      case "graph":
        this.setGraph(message);
        return true;
      case "frame":
        return this.addFrame(message);
      case "summary":
        this.summary = message;
        return true;
      case "error":
        this.failWith(message);
        return false;
    }
  }

  private setGraph(graph: GraphLine): void {
    this.graphs.set(graph.rev, graph);
    this.graph = graph;
    // 5b has one graph per stream (rev 0); a later revision (5a §8: a changed topology) will need
    // columns per revision, which is why frames carry `rev`.
    this.capacity = Math.max(this.run?.frames ?? 0, 16);
    this.t = new Float64Array(this.capacity);
    this.dt = new Float64Array(this.capacity);
    const pipes = graph.links.filter((link) => link.pipe_row !== null).length;
    this.nodes = new Columns(graph.node_columns, graph.nodes.length, this.capacity);
    this.links = new Columns(graph.link_columns, pipes, this.capacity);
    this.ops = new Columns(graph.op_columns, graph.ops.length, this.capacity);
  }

  private addFrame(frame: FrameLine): boolean {
    if (!this.graphs.has(frame.rev) || !this.nodes || !this.links || !this.ops) {
      this.fail({ message: `frame for graph rev ${frame.rev}, which never came` });
      return false;
    }
    if (this.frameCount === this.capacity) this.grow(this.capacity * 2);
    const k = this.frameCount;
    this.t[k] = frame.t_s;
    this.dt[k] = frame.dt_s;
    this.nodes.set(k, frame.nodes);
    this.links.set(k, frame.links);
    this.ops.set(k, frame.ops);
    ++this.frameCount;
    if (++this.sinceNotified >= this.batchFrames) this.notify();
    return true;
  }

  private grow(capacity: number): void {
    const grown = (old: Float64Array) => {
      const next = new Float64Array(capacity);
      next.set(old);
      return next;
    };
    this.t = grown(this.t);
    this.dt = grown(this.dt);
    this.nodes?.grow(capacity);
    this.links?.grow(capacity);
    this.ops?.grow(capacity);
    this.capacity = capacity;
  }

  private failWith(line: ErrorLine): void {
    this.fail({ message: line.message, file: line.file, line: line.line, column: line.column });
  }

  private fail(problem: Problem): void {
    this.problem = problem;
    this.finish("error");
  }

  private finish(status: StoreStatus): void {
    this.status = status;
    this.notify();
  }

  private notify(): void {
    this.sinceNotified = 0;
    ++this.version;
    for (const listener of this.listeners) listener();
  }

  private require(columns: Columns | undefined): Columns {
    if (columns === undefined) throw new Error("no graph yet");
    return columns;
  }
}

// The fluid queue's balance over bucket `frame` of node `row` (5a §6.2): what came in,
// demand · dt, and where it went, (drain + dropped) · dt plus the change of the backlog since the
// previous frame. Rates are the bucket's means and the backlog a level at its end, which is why
// the law still holds per bucket. `previousBacklog` defaults to the previous frame's (0 before the
// first); a looping day passes its last frame's, which the seam check makes equal (spec §10).
export function bucketBalance(
  store: DatasetStore,
  row: number,
  frame: number,
  previousBacklog?: number,
): { inflow: number; outflow: number } {
  const value = (name: string, k: number) => store.nodeColumn(row, name)[k] ?? NaN;
  const dt = store.durations()[frame] ?? NaN;
  const before = previousBacklog ?? (frame > 0 ? value("backlog", frame - 1) : 0);
  return {
    inflow: value("demand_per_s", frame) * dt,
    outflow:
      (value("drain_per_s", frame) + value("dropped_per_s", frame)) * dt +
      value("backlog", frame) -
      before,
  };
}
