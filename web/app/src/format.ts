// How the page writes numbers, shared by the boxes, the charts and the report.

// 45 → "45 s", 225 → "3.8 min", 180 → "3 min", 7560 → "2.1 h" (min, not m: "2.92M" is a count).
export function duration(seconds: number): string {
  if (seconds < 10) return `${Number(seconds.toPrecision(2))} s`;
  if (seconds < 90) return `${Math.round(seconds)} s`;
  if (seconds < 5400) return `${Number((seconds / 60).toFixed(1))} min`;
  return `${Number((seconds / 3600).toFixed(1))} h`;
}

// A latency or a wait: below a second in milliseconds, "2.33 ms" (under a millisecond "< 1 ms":
// a queue that keeps up waits nanoseconds, which only reads as noise); above, as a duration.
export function latency(seconds: number): string {
  if (seconds > 0 && seconds < 0.001) return "< 1 ms";
  if (seconds < 1) return `${Number((seconds * 1000).toPrecision(3))} ms`;
  return duration(seconds);
}

// 2923830 → "2.92M", 50 → "50".
export function count(n: number): string {
  if (n >= 1e9) return `${Number((n / 1e9).toPrecision(3))}G`;
  if (n >= 1e6) return `${Number((n / 1e6).toPrecision(3))}M`;
  if (n >= 1e3) return `${Number((n / 1e3).toPrecision(3))}k`;
  return String(Number(n.toPrecision(3)));
}

// 1500 → "1.5k/s", 2000000 → "2M/s".
export function rate(perSecond: number): string {
  return `${count(perSecond)}/s`;
}

// A share: 0.6094 → "61%", 0.001 → "0.1%", 0 → "0%".
export function percent(share: number): string {
  return `${Number((share * 100).toPrecision(2))}%`;
}
