// A scenario's diagram.yaml (phase 5b): how its author wants the system drawn, checked against the
// graph simsys resolved, so the file cannot name a node or a link the scenario no longer has. The
// precompute copies the checked file into the dataset line; simsys never reads it (its loader
// stays strict about the scenario's own keys).
import type { Diagram, GraphLine, Side } from "@simsys/stream";

const sideNames: readonly Side[] = ["left", "right", "top", "bottom"];
const keys = ["cells", "sides"] as const;

// `raw` as parsed from YAML; `file` names it in the messages. Throws an Error naming the first
// thing wrong.
export function checkDiagram(raw: unknown, graph: GraphLine, file: string): Diagram {
  const fail = (why: string): never => {
    throw new Error(`${file}: ${why}`);
  };
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) fail("must be a map");
  const map = raw as Record<string, unknown>;
  for (const key of Object.keys(map)) {
    if (!(keys as readonly string[]).includes(key))
      fail(`unknown key '${key}' (${keys.join(", ")})`);
  }
  const diagram: {
    cells?: Record<string, [number, number]>;
    sides?: Record<string, [Side, Side]>;
  } = {};
  if (map.cells !== undefined) diagram.cells = checkCells(map.cells, graph, fail);
  if (map.sides !== undefined) diagram.sides = checkSides(map.sides, graph, fail);
  return diagram;
}

function checkCells(
  raw: unknown,
  graph: GraphLine,
  fail: (why: string) => never,
): Record<string, [number, number]> {
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) fail("cells must be a map");
  const ids = new Set(graph.nodes.map((n) => n.id));
  const cells: Record<string, [number, number]> = {};
  const taken = new Map<string, string>();
  for (const [id, value] of Object.entries(raw as Record<string, unknown>)) {
    if (!ids.has(id)) fail(`cells: no node '${id}' in the scenario`);
    const whole = (v: unknown) => Number.isInteger(v) && (v as number) >= 0;
    if (!Array.isArray(value) || value.length !== 2 || !value.every(whole)) {
      fail(`cells.${id} must be [column, row], whole numbers from 0`);
    }
    const cell = value as [number, number];
    const at = `[${cell[0]}, ${cell[1]}]`;
    const other = taken.get(at);
    if (other !== undefined) fail(`cells: ${other} and ${id} are both at ${at}`);
    taken.set(at, id);
    cells[id] = cell;
  }
  // A diagram is placed by hand or by ELK, not half and half: ELK cannot fit boxes around fixed ones.
  const missing = graph.nodes.filter((n) => !(n.id in cells)).map((n) => n.id);
  if (missing.length > 0) {
    fail(
      `cells place ${ids.size - missing.length} of ${ids.size} nodes; also place: ${missing.join(", ")}`,
    );
  }
  return cells;
}

function checkSides(
  raw: unknown,
  graph: GraphLine,
  fail: (why: string) => never,
): Record<string, [Side, Side]> {
  if (typeof raw !== "object" || raw === null || Array.isArray(raw)) fail("sides must be a map");
  const links = new Set(graph.links.map((l) => `${l.from} -> ${l.to}`));
  const sides: Record<string, [Side, Side]> = {};
  for (const [key, value] of Object.entries(raw as Record<string, unknown>)) {
    if (!links.has(key)) fail(`sides: no link '${key}' in the scenario`);
    const isSide = (v: unknown) => sideNames.includes(v as Side);
    if (!Array.isArray(value) || value.length !== 2 || !value.every(isSide)) {
      fail(`sides.${key} must be [source side, target side] of ${sideNames.join(", ")}`);
    }
    sides[key] = value as [Side, Side];
  }
  return sides;
}
