// Small streams built in code, for the rules the real fixtures do not reach: an overloaded queue,
// a killed run, an error line.
import { helloLines } from "./hello";

// A ReadableStream of the given string chunks, as fetch's body decoded to text would deliver them.
export function textStream(chunks: readonly string[]): ReadableStream<string> {
  return new ReadableStream<string>({
    start(controller) {
      for (const chunk of chunks) controller.enqueue(chunk);
      controller.close();
    },
  });
}

// The same as bytes.
export function byteStream(bytes: Uint8Array, chunkSize = 1 << 16): ReadableStream<Uint8Array> {
  return new ReadableStream<Uint8Array>({
    start(controller) {
      for (let at = 0; at < bytes.length; at += chunkSize) {
        controller.enqueue(bytes.slice(at, at + chunkSize));
      }
      controller.close();
    },
  });
}

// hello's run and graph lines, then `frames` frames of `row` for the service and an empty client
// row, then hello's summary unless `summary` is false.
export function helloWith(
  frames: number,
  row: readonly (number | null)[],
  options: { summary?: boolean; dt_s?: number } = {},
): string[] {
  const dt = options.dt_s ?? 30;
  const client = Array<null>(15).fill(null);
  const lines = [helloLines[0] ?? "", helloLines[1] ?? ""];
  for (let k = 0; k < frames; ++k) {
    lines.push(
      JSON.stringify({
        type: "frame",
        rev: 0,
        t_s: k * dt,
        dt_s: dt,
        nodes: [client, row],
        links: [],
        ops: [[100, 100, 100, 0, 1.6, 5.7]],
      }),
    );
  }
  if (options.summary ?? true) lines.push(helloLines[4] ?? "");
  return lines;
}

// A service of 1000/s sent 1500/s with a room of 100 requests, in buckets of 0.1 s (one step): the
// 5a theory test's overload. The backlog grows 50 a bucket ((1500 − 1000) × 0.1) until the room is
// full at 100, then the 500/s excess is dropped. Each frame is the bucket's balance written out:
// demand · dt = (drain + dropped) · dt + backlog − previous backlog.
export function overloadedLines(): string[] {
  const dt = 0.1;
  const client = Array<null>(15).fill(null);
  const lines = [helloLines[0] ?? "", helloLines[1] ?? ""];
  let backlog = 0;
  for (let k = 0; k < 10; ++k) {
    const room = 100 - backlog;
    const excess = (1500 - 1000) * dt;
    const queued = Math.min(excess, room);
    const dropped = (excess - queued) / dt;
    backlog += queued;
    lines.push(
      JSON.stringify({
        type: "frame",
        rev: 0,
        t_s: Number((k * dt).toPrecision(6)),
        dt_s: dt,
        nodes: [
          client,
          [1500, 1000, dropped, 0, backlog, backlog, 1, 1, 2, dt, 100, null, null, null, null],
        ],
        links: [],
        ops: [[1500, 1500, 1000, 0, 100, 150]],
      }),
    );
  }
  lines.push(helloLines[4] ?? "");
  return lines;
}
