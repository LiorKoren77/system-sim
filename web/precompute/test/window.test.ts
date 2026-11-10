import { describe, expect, it } from "vitest";
import { cutWindow } from "../src/window";
import { frame, graph, twoDays } from "./support";

describe("the window a dataset keeps", () => {
  it("keeps the second day of a day that repeats, and loops it", () => {
    const cut = cutWindow("day", graph, twoDays(), 172800);
    expect(cut.frames).toHaveLength(720);
    expect(cut.frames[0]?.t_s).toBe(86400);
    expect(cut.window).toEqual({ from_s: 86400, to_s: 172800 });
    expect(cut.loops).toBe(true);
    expect(cut.seam?.status).toBe("repeats");
  });

  it("keeps both days of a day that drifts, and does not loop it", () => {
    const cut = cutWindow(
      "day",
      graph,
      twoDays((k) => k * 10),
      172800,
    );
    expect(cut.frames).toHaveLength(1440);
    expect(cut.window).toEqual({ from_s: 0, to_s: 172800 });
    expect(cut.loops).toBe(false);
    expect(cut.seam?.status).toBe("drifts");
  });

  it("keeps the whole of a run, with no seam", () => {
    const frames = [frame(0, 0), frame(120, 0), frame(240, 0)];
    const cut = cutWindow("run", graph, frames, 360);
    expect(cut.frames).toHaveLength(3);
    expect(cut.window).toEqual({ from_s: 0, to_s: 360 });
    expect(cut.loops).toBe(false);
    expect(cut.seam).toBeNull();
  });
});
