import { describe, expect, it } from "vitest";
import { seamCheck } from "../src/seam";
import { frame, graph } from "./support";

const day2 = [frame(86400, 0), frame(172740, 0)];

describe("the seam check: does the state at 48:00 equal the state at 24:00", () => {
  it("calls identical frames a day that repeats", () => {
    expect(seamCheck(graph, frame(86340, 0), frame(172740, 0), day2)).toEqual({
      status: "repeats",
      worst: null,
    });
  });

  it("calls a backlog that doubles in a day a drift, naming the node and column", () => {
    const seam = seamCheck(graph, frame(86340, 1000), frame(172740, 2000), day2);
    expect(seam.status).toBe("drifts");
    expect(seam.worst).toEqual({
      row: "node",
      id: "q",
      column: "backlog",
      a: 1000,
      b: 2000,
      rel: 0.5,
    });
  });

  it("ignores a crumb of backlog against none on a node that queues thousands", () => {
    // The absolute term: 1e-6 × the column's day-2 peak, here 5000 → 0.005 > 0.0001.
    const peak = [frame(86400, 5000), frame(172740, 0)];
    expect(seamCheck(graph, frame(86340, 0.0001), frame(172740, 0), peak).status).toBe("repeats");
  });

  it("absorbs the six-digit rounding of a frame's numbers and no more", () => {
    // 1e-3 × 1000.4 ≈ 1.0: a difference of 0.4 passes, one of 2 does not.
    expect(seamCheck(graph, frame(86340, 1000.4), frame(172740, 1000), day2).status).toBe(
      "repeats",
    );
    expect(seamCheck(graph, frame(86340, 1002), frame(172740, 1000), day2).status).toBe("drifts");
  });

  it("treats null against null as equal and null against a number as a drift", () => {
    expect(seamCheck(graph, frame(86340, null), frame(172740, null), day2).status).toBe("repeats");
    const seam = seamCheck(graph, frame(86340, null), frame(172740, 0), day2);
    expect(seam.status).toBe("drifts");
    expect(seam.worst).toMatchObject({ id: "q", column: "backlog", a: null, b: 0 });
  });

  it("checks the op rows as it checks the node rows", () => {
    const a = frame(86340, 0);
    const b = { ...frame(172740, 0), ops: [[900, 900, 900, 0, 2.5, 6]] };
    expect(seamCheck(graph, a, b, day2).worst).toMatchObject({
      row: "op",
      id: "users.read",
      column: "offered_per_s",
    });
  });
});
