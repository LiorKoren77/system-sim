// One simsys child process and its stream: spawned, its stdout cut into lines by the stream
// package's splitter (the one the page uses), each line kept as simsys wrote it beside its parsed
// message, so a dataset carries simsys's bytes unchanged.
import { spawn } from "node:child_process";
import { Readable } from "node:stream";
import {
  parseLine,
  splitLines,
  type FrameLine,
  type GraphLine,
  type RunLine,
  type SummaryLine,
} from "@simsys/stream";

export interface SimsysRun {
  readonly lines: readonly string[];
  readonly code: number;
  readonly stderr: string;
}

export interface Kept<T> {
  readonly text: string;
  readonly message: T;
}

export interface Stream {
  readonly run: Kept<RunLine>;
  readonly graph: Kept<GraphLine>;
  readonly frames: readonly Kept<FrameLine>[];
  readonly summary: Kept<SummaryLine>;
}

// Runs `command args` in `cwd` to its end and collects its stdout's lines.
export async function runSimsys(
  command: string,
  args: readonly string[],
  cwd: string,
): Promise<SimsysRun> {
  const child = spawn(command, args, { cwd, stdio: ["ignore", "pipe", "pipe"] });
  let stderr = "";
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk: string) => (stderr += chunk));
  const exited = new Promise<number>((resolve, reject) => {
    child.on("error", reject);
    child.on("close", (code) => {
      resolve(code ?? -1);
    });
  });
  const text = (
    Readable.toWeb(child.stdout) as ReadableStream<Uint8Array<ArrayBuffer>>
  ).pipeThrough(new TextDecoderStream());
  // Promise.all awaits both at once: a spawn that fails (no such binary) rejects `exited` while the
  // lines are still read, and that rejection must have a handler from the start, or Node reports it
  // as unhandled and stops.
  const [lines, code] = await Promise.all([collect(splitLines(text)), exited]);
  return { lines, code, stderr };
}

async function collect(lines: AsyncIterable<string>): Promise<string[]> {
  const all: string[] = [];
  for await (const line of lines) all.push(line);
  return all;
}

// The stream of a finished run, or an Error saying why it is no dataset: an error line (with the
// file, line and column it names), a line that breaks the contract, or an end without a summary (the
// run was killed). Exit 2 is data: an SLO failed, which the page shows.
export function readStream(result: SimsysRun, scenario: string): Stream {
  let run: Kept<RunLine> | undefined;
  let graph: Kept<GraphLine> | undefined;
  let summary: Kept<SummaryLine> | undefined;
  const frames: Kept<FrameLine>[] = [];
  result.lines.forEach((text, i) => {
    const parsed = parseLine(text, i + 1);
    if (parsed.kind === "ignored") return;
    if (parsed.kind === "error") throw new Error(`${scenario}: ${parsed.message}`);
    const message = parsed.message;
    switch (message.type) {
      case "run":
        run = { text, message };
        break;
      case "graph":
        graph = { text, message };
        break;
      case "frame":
        frames.push({ text, message });
        break;
      case "summary":
        summary = { text, message };
        break;
      case "error":
        throw new Error(
          `${message.file ?? scenario}:${message.line}:${message.column}: ${message.message}`,
        );
      case "dataset":
        break; // simsys never writes one
    }
  });
  if (summary === undefined || run === undefined || graph === undefined) {
    const stderr = result.stderr === "" ? "" : `\n${result.stderr}`;
    throw new Error(
      `${scenario}: the stream ended without a summary (exit ${result.code})${stderr}`,
    );
  }
  return { run, graph, frames, summary };
}
