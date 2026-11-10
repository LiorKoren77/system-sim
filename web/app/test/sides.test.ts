import { describe, expect, it } from "vitest";
import { sidesFor } from "../src/diagram/sides";

// Boxes of the layout's size (176 × 68), centre to centre.
const box = (x: number, y: number) => ({ x, y, width: 176, height: 68 });

describe("which sides of two boxes an edge joins", () => {
  it("joins a box to the right right side to left side: the flow's direction", () => {
    expect(sidesFor(box(0, 0), box(240, 30))).toEqual({ source: "right", target: "left" });
  });

  it("joins a box below bottom to top, and a box above top to bottom", () => {
    expect(sidesFor(box(0, 0), box(40, 200))).toEqual({ source: "bottom", target: "top" });
    expect(sidesFor(box(0, 200), box(40, 0))).toEqual({ source: "top", target: "bottom" });
  });

  it("loops an edge back to a box on the left underneath, bottom to bottom", () => {
    // web-crawler's url_seen → frontier: newly seen URLs go back to the start of the pipeline.
    expect(sidesFor(box(800, 200), box(160, 70))).toEqual({ source: "bottom", target: "bottom" });
  });

  it("loops back over the top when the box on the left is lower", () => {
    expect(sidesFor(box(800, 0), box(160, 130))).toEqual({ source: "top", target: "top" });
  });

  it("weighs the gap between the boxes, not their centres: wide boxes side by side are horizontal", () => {
    // Centres 200 apart across and 100 down, but the boxes overlap vertically less than across.
    expect(sidesFor(box(0, 0), box(200, 100))).toEqual({ source: "right", target: "left" });
    // Stacked with a small horizontal offset: vertical.
    expect(sidesFor(box(0, 0), box(60, 100))).toEqual({ source: "bottom", target: "top" });
  });
});
