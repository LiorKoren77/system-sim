import { describe, expect, it } from "vitest";
import { groupLabel } from "../src/catalogue/group-label";

describe("the label a catalogue group is shown under", () => {
  it("shows the book folder as systems", () => {
    expect(groupLabel("book")).toBe("systems");
  });

  it("leaves the other folders' names as they are", () => {
    expect(groupLabel("labs")).toBe("labs");
    expect(groupLabel("tutorial")).toBe("tutorial");
  });
});
