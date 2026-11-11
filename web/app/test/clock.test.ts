import { describe, expect, it } from "vitest";
import { clockLabel, durationLabel } from "../src/timeline/clock";

const day = { kind: "day", window: { from_s: 86400, to_s: 172800 } } as const;
const run = { kind: "run", window: { from_s: 0, to_s: 7200 } } as const;

describe("the clock the slider shows", () => {
  it("reads a day's frames as the time of day, from midnight", () => {
    // A frame is a 1 min bucket; frame k starts at 86400 + 60 k.
    expect(clockLabel(86400, day)).toBe("00:00");
    expect(clockLabel(86400 + 60, day)).toBe("00:01");
    expect(clockLabel(86400 + 1439 * 60, day)).toBe("23:59");
  });

  it("reads a day that does not repeat, kept whole, as day and time", () => {
    const both = { kind: "day", window: { from_s: 0, to_s: 172800 } } as const;
    expect(clockLabel(0, both)).toBe("day 1 00:00");
    expect(clockLabel(86400 + 3600, both)).toBe("day 2 01:00");
  });

  it("reads a run's frames as time since it started", () => {
    expect(clockLabel(3900, run)).toBe("1h 05m");
    expect(clockLabel(125, run)).toBe("2m 05s");
    expect(clockLabel(12.5, run)).toBe("12.5s");
    expect(clockLabel(0, run)).toBe("0s");
  });

  it("names a run's length for the catalogue", () => {
    expect(durationLabel(1800)).toBe("30 min");
    expect(durationLabel(21600)).toBe("6 h");
    expect(durationLabel(60)).toBe("1 min");
    expect(durationLabel(90)).toBe("90 s");
  });
});
