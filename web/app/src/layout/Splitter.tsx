import { useRef, type RefObject } from "react";
import {
  SIDE_DEFAULT,
  SIDE_MIN,
  maxSideWidth,
  sideWidthAfterDrag,
  sideWidthAfterKey,
} from "./panes";

// The border between the diagram and the charts panel: dragged, moved with the arrow keys when it
// has the focus, and put back to the first width by a double click. A `separator` that can be
// focused is what a screen reader calls a window splitter; its value is the panel's width in px.
export function Splitter(props: {
  width: number;
  // The stage the two panes share: its width bounds the panel. Read when a move starts, not kept in
  // state, so a window resize needs no listener.
  stage: RefObject<HTMLDivElement | null>;
  onResize: (width: number) => void;
}) {
  const { width, stage, onResize } = props;
  // useRef, not useState: where the drag began changes nothing on screen, so it must not render.
  const drag = useRef<{ width: number; x: number } | null>(null);
  const stageWidth = () => stage.current?.clientWidth ?? 0;
  const max = maxSideWidth(stageWidth());

  return (
    <div
      className="splitter"
      role="separator"
      aria-orientation="vertical"
      aria-label="Resize the charts panel"
      aria-valuenow={width}
      aria-valuemin={SIDE_MIN}
      {...(Number.isFinite(max) ? { "aria-valuemax": max } : {})}
      tabIndex={0}
      title="Drag to resize; double-click for the first width"
      onPointerDown={(e) => {
        drag.current = { width, x: e.clientX };
        // Capture: the moves keep coming here when the pointer leaves the 6 px bar, over the
        // diagram or a chart, which would otherwise take them.
        e.currentTarget.setPointerCapture(e.pointerId);
        e.preventDefault(); // no text selection while dragging
      }}
      onPointerMove={(e) => {
        if (drag.current !== null)
          onResize(sideWidthAfterDrag(drag.current, e.clientX, stageWidth()));
      }}
      onPointerUp={() => {
        drag.current = null;
      }}
      onPointerCancel={() => {
        drag.current = null;
      }}
      onDoubleClick={() => {
        onResize(SIDE_DEFAULT);
      }}
      onKeyDown={(e) => {
        const next = sideWidthAfterKey(e.key, width, stageWidth());
        if (next === null) return;
        // The page's own keys (← → step the day, Home rewinds it) must not also fire.
        e.preventDefault();
        e.stopPropagation();
        onResize(next);
      }}
    />
  );
}
