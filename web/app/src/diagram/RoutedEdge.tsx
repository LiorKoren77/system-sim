import { BaseEdge, getSmoothStepPath, type Edge, type EdgeProps } from "@xyflow/react";
import { useContext } from "react";
import { FlowContext } from "./frame-context";
import type { Point } from "./layout";

export type RoutedEdgeData = { readonly index: number; readonly points?: readonly Point[] };

// A link of the diagram. Its line follows the route ELK computed (layout.ts), right-angled segments
// around the boxes; where there is none (a diagram placed by hand), right-angled segments between
// the two connection points. Its dashes move toward the target at the speed of the traffic the
// link carries in the frame (flow.ts), read from a context so that a new frame changes only the
// dashes' speed, not the edge React Flow holds.
export function RoutedEdge(props: EdgeProps<Edge<RoutedEdgeData>>) {
  const { id, data, markerEnd, markerStart, style } = props;
  const duration = useContext(FlowContext)[data?.index ?? -1] ?? null;
  const points = data?.points;
  const path =
    points && points.length >= 2
      ? points.map((p, i) => `${i === 0 ? "M" : "L"}${p.x},${p.y}`).join(" ")
      : getSmoothStepPath(props)[0];
  return (
    <BaseEdge
      id={id}
      path={path}
      {...(markerEnd ? { markerEnd } : {})}
      {...(markerStart ? { markerStart } : {})}
      className={duration === null ? "" : "flowing"}
      style={{ ...style, ...(duration === null ? {} : { animationDuration: `${duration}s` }) }}
    />
  );
}
