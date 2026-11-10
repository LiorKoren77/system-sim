import { BaseEdge, type Edge, type EdgeProps } from "@xyflow/react";
import type { Point } from "./layout";

export type RoutedEdgeData = { readonly points: readonly Point[] };

// An edge drawn along the route ELK computed (layout.ts): straight segments at right angles that
// go around the boxes in between. React Flow would otherwise draw its own line between the two
// connection points, blind to the boxes it crosses.
export function RoutedEdge({ id, data, markerEnd, style }: EdgeProps<Edge<RoutedEdgeData>>) {
  const points = data?.points ?? [];
  const path = points.map((p, i) => `${i === 0 ? "M" : "L"}${p.x},${p.y}`).join(" ");
  return (
    <BaseEdge
      id={id}
      path={path}
      {...(markerEnd ? { markerEnd } : {})}
      {...(style ? { style } : {})}
    />
  );
}
