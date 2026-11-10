// The frames a dataset keeps (phase 5b spec §6): a day that repeats keeps its second day and loops
// it; a day that drifts keeps both days, to show the drift, and plays once; a run keeps everything.
import type { FrameLine, GraphLine, Seam } from "@simsys/stream";
import { seamCheck } from "./seam";

const day_s = 86400;

export interface Window {
  readonly frames: readonly FrameLine[];
  readonly frameTexts: readonly string[]; // the kept frames' lines as simsys wrote them
  readonly window: { readonly from_s: number; readonly to_s: number };
  readonly loops: boolean;
  readonly seam: Seam | null;
}

export function cutWindow(
  kind: "day" | "run",
  graph: GraphLine,
  frames: readonly FrameLine[],
  duration_s: number,
  texts: readonly string[] = frames.map((f) => JSON.stringify(f)),
): Window {
  const whole = {
    frames,
    frameTexts: texts,
    window: { from_s: 0, to_s: duration_s },
    loops: false,
  };
  if (kind === "run") return { ...whole, seam: null };
  const firstOfDay2 = frames.findIndex((f) => f.t_s >= day_s);
  const endOfDay1 = frames[firstOfDay2 - 1];
  const endOfDay2 = frames.at(-1);
  if (firstOfDay2 < 1 || endOfDay1 === undefined || endOfDay2 === undefined) {
    throw new Error(`a day lived twice needs frames on both days (got ${frames.length})`);
  }
  const day2 = frames.slice(firstOfDay2);
  const seam = seamCheck(graph, endOfDay1, endOfDay2, day2);
  if (seam.status === "drifts") return { ...whole, seam };
  return {
    frames: day2,
    frameTexts: texts.slice(firstOfDay2),
    window: { from_s: day_s, to_s: duration_s },
    loops: true,
    seam,
  };
}
