import type { DatasetStore } from "@simsys/stream";
import { useEffect, useMemo, useRef } from "react";
import { frameAtX, NO_ZONE, rowAtY, zoneGrid } from "./zone-strip";

const ROW_HEIGHT = 12; // px per node

function rgb(name: string): [number, number, number] {
  const hex = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const n = Number.parseInt(hex.replace("#", ""), 16);
  return Number.isNaN(n) ? [128, 128, 128] : [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

// The window's health at a glance, under the slider: a row per node, a cell per frame, in the
// frame's zone colour ("red at 21:30 for two buckets, then yellow for half an hour"). The canvas
// holds one pixel per cell and the page stretches it, so a day of 1440 frames costs 1440 × rows
// pixels whatever the screen. A row's name selects the node; a click on a cell also moves the
// slider there. A line marks the slider's frame.
export function ZoneStrip(props: {
  store: DatasetStore;
  frame: number;
  selected: string | null;
  onSelect: (id: string) => void;
  onSeek: (frame: number) => void;
}) {
  const { store, frame, selected, onSelect, onSeek } = props;
  const graph = store.graph;
  const canvas = useRef<HTMLCanvasElement>(null);
  // Built once the dataset has loaded (the boxes' scales are the whole dataset's: bars.ts).
  const ready = store.status === "ready" ? store.version : -1;
  const grid = useMemo(
    () => (graph && ready >= 0 ? zoneGrid(graph, store) : null),
    [graph, store, ready],
  );

  useEffect(() => {
    const element = canvas.current;
    const context = element?.getContext("2d");
    if (!element || !context || !grid || grid.frames === 0 || grid.rows.length === 0) return;
    // Green is the quiet dark green of a box's background, yellow and red at full strength: a calm
    // day reads as a dark strip and trouble stands out, as in the diagram.
    const colours = [rgb("--zone-green-bg"), rgb("--zone-yellow"), rgb("--zone-red")];
    const none = rgb("--panel");
    const image = context.createImageData(grid.frames, grid.rows.length);
    grid.cells.forEach((zone, i) => {
      const [r, g, b] = zone === NO_ZONE ? none : (colours[zone] ?? none);
      image.data.set([r, g, b, 255], i * 4);
    });
    context.putImageData(image, 0, 0);
  }, [grid]);

  if (!grid || grid.rows.length === 0) return null;
  const at = grid.frames > 0 ? ((frame + 0.5) / grid.frames) * 100 : 0;
  return (
    <div className="zone-strip">
      <div className="names">
        {grid.rows.map((row) => (
          <button
            key={row.id}
            type="button"
            aria-current={row.id === selected}
            style={{ height: ROW_HEIGHT }}
            onClick={() => {
              onSelect(row.id);
            }}
          >
            {row.id}
          </button>
        ))}
      </div>
      <div className="cells">
        <canvas
          ref={canvas}
          width={grid.frames}
          height={grid.rows.length}
          style={{ height: grid.rows.length * ROW_HEIGHT }}
          aria-label="Zones over the window: a row per node, a column per frame"
          onClick={(e) => {
            const box = e.currentTarget.getBoundingClientRect();
            const r = rowAtY(e.clientY - box.top, ROW_HEIGHT, grid.rows.length);
            const row = r === null ? undefined : grid.rows[r];
            if (row) onSelect(row.id);
            onSeek(frameAtX(e.clientX - box.left, box.width, grid.frames));
          }}
        />
        <div className="strip-cursor" style={{ left: `${at}%` }} />
      </div>
    </div>
  );
}
