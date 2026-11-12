import { describe, expect, it } from "vitest";
import {
  DIAGRAM_MIN,
  SIDE_DEFAULT,
  SIDE_MIN,
  clampSideWidth,
  defaultPanes,
  parsePanes,
  sideWidthAfterDrag,
  sideWidthAfterKey,
} from "../src/layout/panes";

describe("the width of the charts panel", () => {
  it("never leaves the diagram less than its minimum", () => {
    expect(clampSideWidth(900, 1000)).toBe(1000 - DIAGRAM_MIN);
  });

  it("never goes under the width its charts need", () => {
    expect(clampSideWidth(50, 1000)).toBe(SIDE_MIN);
  });

  it("keeps its minimum on a stage too narrow for both", () => {
    expect(clampSideWidth(400, SIDE_MIN + 10)).toBe(SIDE_MIN);
  });

  it("is bounded only from below while the stage has not been measured", () => {
    expect(clampSideWidth(5000, 0)).toBe(5000);
    expect(clampSideWidth(10, 0)).toBe(SIDE_MIN);
  });

  it("grows by what the splitter was dragged to the left, and shrinks to the right", () => {
    expect(sideWidthAfterDrag({ width: 420, x: 800 }, 700, 1400)).toBe(520);
    expect(sideWidthAfterDrag({ width: 420, x: 800 }, 850, 1400)).toBe(370);
  });

  it("stops at its bounds however far the pointer goes", () => {
    expect(sideWidthAfterDrag({ width: 420, x: 800 }, -5000, 1400)).toBe(1400 - DIAGRAM_MIN);
    expect(sideWidthAfterDrag({ width: 420, x: 800 }, 5000, 1400)).toBe(SIDE_MIN);
  });

  it("moves with the arrow keys as the splitter would, and goes to an end with Home and End", () => {
    const left = sideWidthAfterKey("ArrowLeft", 420, 1400);
    const right = sideWidthAfterKey("ArrowRight", 420, 1400);
    expect(left).toBeGreaterThan(420);
    expect(right).toBeLessThan(420);
    expect(sideWidthAfterKey("Home", 420, 1400)).toBe(1400 - DIAGRAM_MIN);
    expect(sideWidthAfterKey("End", 420, 1400)).toBe(SIDE_MIN);
  });

  it("leaves every other key to the page", () => {
    expect(sideWidthAfterKey(" ", 420, 1400)).toBeNull();
    expect(sideWidthAfterKey("a", 420, 1400)).toBeNull();
  });
});

describe("the panes a viewer left", () => {
  it("come back as they were saved", () => {
    const saved = { catalogueOpen: false, stripOpen: false, sideWidth: 600 };
    expect(parsePanes(JSON.stringify(saved))).toEqual(saved);
  });

  it("are the defaults when nothing was saved", () => {
    expect(parsePanes(null)).toEqual(defaultPanes);
    expect(defaultPanes).toEqual({
      catalogueOpen: true,
      stripOpen: true,
      sideWidth: SIDE_DEFAULT,
    });
  });

  it("are the defaults for what is not a saved layout", () => {
    expect(parsePanes("not json")).toEqual(defaultPanes);
    expect(parsePanes("[1,2]")).toEqual(defaultPanes);
    expect(parsePanes("null")).toEqual(defaultPanes);
  });

  it("keep the valid part of a half-valid layout", () => {
    expect(parsePanes(JSON.stringify({ catalogueOpen: false, sideWidth: "wide" }))).toEqual({
      catalogueOpen: false,
      stripOpen: true,
      sideWidth: SIDE_DEFAULT,
    });
    expect(
      parsePanes(JSON.stringify({ catalogueOpen: "no", stripOpen: false, sideWidth: 10 })),
    ).toEqual({ catalogueOpen: true, stripOpen: false, sideWidth: SIDE_MIN });
  });
});
