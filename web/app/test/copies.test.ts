import { readFileSync } from "node:fs";
import path from "node:path";
import type { GraphLine } from "@simsys/stream";
import { describe, expect, it } from "vitest";
import { copiesAt } from "../src/diagram/copies";

// The consistent-hashing lab: 4 cache servers, one of them down from 10 m for 5 m.
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

// A frame is a bucket [t, t + dt): the lab's buckets are 1.7 s (1200 s of 0.1 s steps in 706 frames,
// rounded to whole steps); the tests use 2 s, the rule is the same.
const dt = 2;

describe("how many copies of a node are up in a frame", () => {
  it("has all four cache servers up before the fault", () => {
    expect(copiesAt(lab, 598, dt).get("cache_servers")).toEqual({
      up: 4,
      of: 4,
      primaryDown: false,
    });
  });

  it("has three of four up while the fault takes one", () => {
    expect(copiesAt(lab, 600, dt).get("cache_servers")).toEqual({
      up: 3,
      of: 4,
      primaryDown: false,
    });
    expect(copiesAt(lab, 898, dt).get("cache_servers")).toEqual({
      up: 3,
      of: 4,
      primaryDown: false,
    });
  });

  it("counts a fault that starts inside a bucket as on in that bucket", () => {
    // [599, 601) holds the fault's first second: the zone of that bucket is its worst step too.
    expect(copiesAt(lab, 599, dt).get("cache_servers")?.up).toBe(3);
  });

  it("has all four back when the fault has cleared", () => {
    expect(copiesAt(lab, 900, dt).get("cache_servers")?.up).toBe(4);
  });

  it("takes every copy down when the fault names no count, and adds overlapping faults up", () => {
    const graph = {
      ...lab,
      faults: [
        { kind: "node_down", target: "db", at_s: 0, for_s: 10, replicas: null, primary: false },
        {
          kind: "node_down",
          target: "cache_servers",
          at_s: 0,
          for_s: 10,
          replicas: 1,
          primary: false,
        },
        {
          kind: "node_down",
          target: "cache_servers",
          at_s: 5,
          for_s: 10,
          replicas: 2,
          primary: false,
        },
      ],
    } as GraphLine;
    expect(copiesAt(graph, 0, dt).get("db")).toMatchObject({ up: 0, of: 2 });
    expect(copiesAt(graph, 6, dt).get("cache_servers")?.up).toBe(1);
  });

  it("marks a sql_db whose primary is down, its copies still serving reads", () => {
    const graph = {
      ...lab,
      faults: [
        { kind: "node_down", target: "db", at_s: 0, for_s: 10, replicas: null, primary: true },
      ],
    } as GraphLine;
    expect(copiesAt(graph, 0, dt).get("db")).toEqual({ up: 1, of: 2, primaryDown: true });
  });

  it("takes every node of a datacenter down with it", () => {
    const dc = lab.nodes[0]?.datacenter ?? "";
    const graph = {
      ...lab,
      faults: [{ kind: "dc_down", target: dc, at_s: 0, for_s: 10, replicas: null, primary: false }],
    } as GraphLine;
    expect(copiesAt(graph, 0, dt).get("cache_servers")?.up).toBe(0);
    expect(copiesAt(graph, 0, dt).get("clients")).toBeUndefined(); // a client keeps sending
  });
});
