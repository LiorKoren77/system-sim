import { describe, expect, it } from "vitest";
import { FORMAT_VERSION } from "../src/index";

describe("the stream package", () => {
  it("exports the format version of the simulator's output it reads", () => {
    expect(FORMAT_VERSION).toBe(1);
  });
});
