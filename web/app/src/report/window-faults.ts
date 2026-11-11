// The faults the page shows: those inside the window it plays. A day is run twice (`--days 2`,
// spec §5) and each scripted fault happens on both days, so the report, which judges day 2,
// also lists day 1's copy, whose effects end before the window starts. A run's window is the
// whole run.
import type { FaultBlock } from "@simsys/stream";

export function faultsInWindow(
  faults: readonly FaultBlock[],
  window: { readonly from_s: number; readonly to_s: number },
): FaultBlock[] {
  return faults.filter((f) => f.at_s >= window.from_s && f.at_s < window.to_s);
}

// The frame whose bucket holds t_s: the last that starts at or before it (the first before the
// window starts, the last after it ends).
export function frameAt(times: Float64Array, t_s: number): number {
  let lo = 0;
  let hi = times.length - 1;
  if (hi < 0) return 0;
  while (lo < hi) {
    const mid = Math.ceil((lo + hi) / 2);
    if ((times[mid] ?? Infinity) <= t_s) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}
