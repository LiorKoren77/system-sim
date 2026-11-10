import { describe, expect, it } from "vitest";
import { parseLine } from "../src/parse";
import { helloLines } from "./support/hello";

describe("reading a line by the output contract", () => {
  it("reads each of hello's lines as the message its type names", () => {
    const types = helloLines.map((line, i) => {
      const parsed = parseLine(line, i + 1);
      return parsed.kind === "message" ? parsed.message.type : parsed.kind;
    });
    expect(types).toEqual(["run", "graph", "frame", "frame", "summary"]);
  });

  it("ignores a line type it does not know (rule 1)", () => {
    expect(parseLine('{"type":"command","op":"node_down"}', 7)).toEqual({ kind: "ignored" });
  });

  it("drops a key it does not know (rule 1)", () => {
    const parsed = parseLine(
      '{"type":"frame","rev":0,"t_s":0,"dt_s":1,"nodes":[],"links":[],"ops":[],"colour":"blue"}',
      3,
    );
    expect(parsed.kind).toBe("message");
    if (parsed.kind === "message") expect(parsed.message).not.toHaveProperty("colour");
  });

  it("refuses another format_version, saying how to rebuild the data", () => {
    const parsed = parseLine(
      helloLines[0]?.replace('"format_version":1', '"format_version":2') ?? "",
      1,
    );
    expect(parsed).toEqual({
      kind: "error",
      message:
        "this page reads format_version 1; the data says 2: rebuild the datasets (npm run precompute)",
    });
  });

  it("keeps a dataset's diagram, the scenario's hand-made layout", () => {
    const parsed = parseLine(
      '{"type":"dataset","dataset_version":1,"id":"x","kind":"run","window":{"from_s":0,"to_s":60},"loops":false,"diagram":{"cells":{"a":[0,0]}}}',
      1,
    );
    expect(parsed.kind === "message" && parsed.message).toMatchObject({
      diagram: { cells: { a: [0, 0] } },
    });
  });

  it("refuses another dataset_version likewise", () => {
    const parsed = parseLine('{"type":"dataset","dataset_version":3,"id":"x"}', 1);
    expect(parsed).toEqual({
      kind: "error",
      message:
        "this page reads dataset_version 1; the data says 3: rebuild the datasets (npm run precompute)",
    });
  });

  it("names the line that is not JSON", () => {
    expect(parseLine('{"type":"frame",', 12)).toEqual({
      kind: "error",
      message: "line 12 is not JSON",
    });
  });

  it("names the key a message cannot do without", () => {
    expect(parseLine('{"type":"frame","rev":0,"t_s":0,"dt_s":1,"links":[],"ops":[]}', 5)).toEqual({
      kind: "error",
      message: "line 5: a frame needs nodes",
    });
  });
});
