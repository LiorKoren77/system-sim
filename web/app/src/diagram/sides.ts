// Which sides of two boxes an edge joins. Every box has a connection point on each side
// (NodeCard.tsx), and an edge takes the pair that faces the other box, so it stays short:
//
// - the other box to the right: right side to left side, the direction the diagram flows in;
// - the other box above or below: top or bottom to the facing side;
// - the other box to the left, an edge back against the flow (web-crawler's url_seen → frontier,
//   a loop): it leaves and enters on the bottom, and runs back under the boxes between, as the book
//   draws such loops; on the top when the box it returns to is lower.
//
// "Above or below" against "beside" is decided by the gap between the two boxes on each axis, not
// by the distance between their centres: the boxes are much wider than tall.

export type Side = "left" | "right" | "top" | "bottom";

export interface Box {
  readonly x: number; // top left corner, in the diagram's coordinates
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

export function sidesFor(source: Box, target: Box): { source: Side; target: Side } {
  const dx = target.x + target.width / 2 - (source.x + source.width / 2);
  const dy = target.y + target.height / 2 - (source.y + source.height / 2);
  const gapAcross = Math.abs(dx) - (source.width + target.width) / 2;
  const gapDown = Math.abs(dy) - (source.height + target.height) / 2;
  if (gapAcross >= gapDown) {
    if (dx >= 0) return { source: "right", target: "left" };
    return dy <= 0 ? { source: "bottom", target: "bottom" } : { source: "top", target: "top" };
  }
  return dy >= 0 ? { source: "bottom", target: "top" } : { source: "top", target: "bottom" };
}
