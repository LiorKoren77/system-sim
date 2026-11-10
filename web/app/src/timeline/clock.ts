// What the slider's clock says for a frame. A frame is a bucket of steps (5a §6.2: a day's is
// 2 min), so the clock names the bucket's start and the page never interpolates between frames:
// there is no state between two buckets to show, only the bucket the time is in.

const day_s = 86400;

interface Shown {
  readonly kind: "day" | "run";
  readonly window: { readonly from_s: number; readonly to_s: number };
}

const twoDigits = (n: number) => String(n).padStart(2, "0");

// A day: the time of day, `HH:MM`, from the window's midnight (a looping day's window is the second
// day, 24:00–48:00 of the run); a day kept whole because it does not repeat adds which day. A run:
// the time since it started.
export function clockLabel(t_s: number, shown: Shown): string {
  if (shown.kind === "run") return runTime(t_s);
  const since = t_s - shown.window.from_s;
  const ofDay = ((since % day_s) + day_s) % day_s;
  const hhmm = `${twoDigits(Math.floor(ofDay / 3600))}:${twoDigits(Math.floor((ofDay % 3600) / 60))}`;
  const days = Math.round((shown.window.to_s - shown.window.from_s) / day_s);
  return days > 1 ? `day ${Math.floor(since / day_s) + 1} ${hhmm}` : hhmm;
}

function runTime(t_s: number): string {
  const h = Math.floor(t_s / 3600);
  const m = Math.floor((t_s % 3600) / 60);
  const s = t_s % 60;
  if (h > 0) return `${h}h ${twoDigits(m)}m`;
  if (m > 0) return `${m}m ${twoDigits(Math.floor(s))}s`;
  return `${Number(s.toFixed(1))}s`;
}

// A run's length, for the catalogue: `30 min`, `6 h`, `90 s`.
export function durationLabel(seconds: number): string {
  if (seconds >= 3600 && seconds % 3600 === 0) return `${seconds / 3600} h`;
  if (seconds >= 60 && seconds % 60 === 0) return `${seconds / 60} min`;
  return `${seconds} s`;
}
