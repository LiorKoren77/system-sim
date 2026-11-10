import { readFileSync } from "node:fs";
import path from "node:path";
import type { GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { timelineMarks } from "../src/timeline/marks";

const lab = {
  type: "graph",
  rev: 0,
  ...(JSON.parse(
    readFileSync(
      path.resolve(import.meta.dirname, "fixtures/consistent-hashing-graph.json"),
      "utf8",
    ),
  ) as object),
} as GraphLine;

// The lab with a 20x surge on its client at 05:00 for 2 min, and a scripted traffic_spike.
const surging = {
  ...lab,
  nodes: lab.nodes.map((n) =>
    n.type === "client" ? { ...n, spikes: [{ at_s: 300, for_s: 120, factor: 20 }] } : n,
  ),
  faults: [
    ...lab.faults,
    {
      kind: "traffic_spike",
      target: "clients",
      at_s: 1000,
      for_s: 60,
      replicas: null,
      primary: false,
    },
  ],
} as GraphLine;

const whole = { from_s: 0, to_s: 1200 };

describe("the marks on the time slider", () => {
  it("marks a scripted fault as a failure, saying what broke", () => {
    expect(timelineMarks(lab, whole)).toEqual([
      { kind: "failure", at_s: 600, for_s: 300, label: "node_down cache_servers (1 of 4)" },
    ]);
  });

  it("marks a rate spike and a traffic_spike fault as a surge of load, not a failure", () => {
    const marks = timelineMarks(surging, whole);
    expect(marks.filter((m) => m.kind === "surge")).toEqual([
      { kind: "surge", at_s: 300, for_s: 120, label: "×20 traffic from clients" },
      { kind: "surge", at_s: 1000, for_s: 60, label: "traffic_spike clients" },
    ]);
  });

  it("marks only what happens in the window the page plays", () => {
    expect(timelineMarks(surging, { from_s: 500, to_s: 1200 }).map((m) => m.at_s)).toEqual([
      600, 1000,
    ]);
  });
});
