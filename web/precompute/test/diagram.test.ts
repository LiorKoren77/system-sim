import { describe, expect, it } from "vitest";
import { checkDiagram } from "../src/diagram";
import { graph } from "./support";

// The test graph: a client `users` and a service `q`, one link users -> q.
const file = "scenarios/x/diagram.yaml";

describe("a scenario's diagram.yaml, checked against its graph", () => {
  it("accepts cells for every node and sides for an existing link", () => {
    const diagram = checkDiagram(
      { cells: { users: [0, 0], q: [1, 0] }, sides: { "users -> q": ["right", "left"] } },
      graph,
      file,
    );
    expect(diagram).toEqual({
      cells: { users: [0, 0], q: [1, 0] },
      sides: { "users -> q": ["right", "left"] },
    });
  });

  it("accepts sides alone, leaving the places to ELK", () => {
    expect(checkDiagram({ sides: { "users -> q": ["bottom", "top"] } }, graph, file)).toEqual({
      sides: { "users -> q": ["bottom", "top"] },
    });
  });

  it("refuses a cell for a node the scenario does not have", () => {
    expect(() =>
      checkDiagram({ cells: { users: [0, 0], q: [1, 0], db: [2, 0] } }, graph, file),
    ).toThrow("scenarios/x/diagram.yaml: cells: no node 'db' in the scenario");
  });

  it("refuses cells that leave a node out: a diagram is placed by hand or by ELK, not half and half", () => {
    expect(() => checkDiagram({ cells: { users: [0, 0] } }, graph, file)).toThrow(
      "scenarios/x/diagram.yaml: cells place 1 of 2 nodes; also place: q",
    );
  });

  it("refuses two nodes in one cell", () => {
    expect(() => checkDiagram({ cells: { users: [0, 0], q: [0, 0] } }, graph, file)).toThrow(
      "scenarios/x/diagram.yaml: cells: users and q are both at [0, 0]",
    );
  });

  it("refuses a cell that is not two whole numbers from 0", () => {
    expect(() => checkDiagram({ cells: { users: [0, -1], q: [1, 0] } }, graph, file)).toThrow(
      "scenarios/x/diagram.yaml: cells.users must be [column, row], whole numbers from 0",
    );
  });

  it("refuses sides for a link the scenario does not have, and a side that is not one", () => {
    expect(() => checkDiagram({ sides: { "q -> users": ["right", "left"] } }, graph, file)).toThrow(
      "scenarios/x/diagram.yaml: sides: no link 'q -> users' in the scenario",
    );
    expect(() => checkDiagram({ sides: { "users -> q": ["right", "up"] } }, graph, file)).toThrow(
      "scenarios/x/diagram.yaml: sides.users -> q must be [source side, target side] of left, right, top, bottom",
    );
  });

  it("refuses a key it does not know", () => {
    expect(() => checkDiagram({ colours: {} }, graph, file)).toThrow(
      "scenarios/x/diagram.yaml: unknown key 'colours' (cells, sides)",
    );
  });
});
