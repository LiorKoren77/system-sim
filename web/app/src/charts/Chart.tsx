import { useEffect, useRef } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import { count, percent } from "../format";
import { toAligned, type ChartSpec, type SeriesRole } from "./series";

// The data series' colours, in a fixed order (the first series is always blue, the second orange,
// ...): four hues checked for the dark panel and for colour blindness as a set (lightness band,
// separation under deutan, protan and tritan simulation, contrast against #222427). A threshold is
// drawn in its zone's colour, dashed, and named in the legend, so colour is never the only signal.
const DATA_COLOURS = ["#3987e5", "#d95926", "#199e70", "#9085e9"] as const;

function cssVar(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888";
}

function stroke(role: SeriesRole, dataIndex: number): string {
  switch (role) {
    case "data":
      return DATA_COLOURS[dataIndex % DATA_COLOURS.length] ?? "#888";
    case "yellow":
      return cssVar("--zone-yellow");
    case "red":
      return cssVar("--zone-red");
    case "limit":
      return cssVar("--muted");
  }
}

function valueLabel(unit: string, v: number | null): string {
  if (v === null) return "—";
  return unit === "share" ? percent(v) : count(v);
}

const HEIGHT = 130;
// Ticks on whole seconds, minutes and hours, not uPlot's powers of ten (500 s would read "8m 20s").
const TIME_INCREMENTS = [
  0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200,
  86400,
];

// One chart, drawn by uPlot (a canvas, fast over thousands of points) in a React component: created
// on mount, given new data when the series change, destroyed on unmount, resized with its box.
// Pattern: Adapter — uPlot's imperative API behind a declarative component. A line at the slider's
// frame moves without redrawing the chart; a click on the chart moves the slider there. The legend
// gives every series' value under the pointer, and the charts share the pointer (uPlot's sync).
export function Chart(props: {
  spec: ChartSpec;
  x: Float64Array;
  frame: number;
  xLabel: (x: number) => string;
  onSeek: (frame: number) => void;
}) {
  const { spec, x, frame, xLabel, onSeek } = props;
  const box = useRef<HTMLDivElement>(null);
  const plot = useRef<uPlot | null>(null);
  const cursor = useRef<HTMLDivElement | null>(null);
  const seek = useRef(onSeek);
  useEffect(() => {
    seek.current = onSeek;
  }, [onSeek]);

  useEffect(() => {
    const element = box.current;
    if (element === null) return;
    const grid = { stroke: cssVar("--line"), width: 1 };
    const axis = { stroke: cssVar("--muted"), grid, ticks: grid, font: "11px system-ui" };
    let dataIndex = 0;
    const options: uPlot.Options = {
      width: element.clientWidth || 360,
      height: HEIGHT,
      cursor: { sync: { key: "simsys" }, points: { show: false } },
      scales: {
        x: { time: false },
        y: {
          range: (_u, _min, max) => [
            0,
            spec.unit === "share" ? Math.max(1, max) : max > 0 ? max * 1.08 : 1,
          ],
        },
      },
      axes: [
        {
          ...axis,
          incrs: TIME_INCREMENTS,
          values: (_u, splits) => splits.map((s) => xLabel(s)),
        },
        {
          ...axis,
          size: 52,
          values: (_u, splits) => splits.map((s) => valueLabel(spec.unit, s)),
        },
      ],
      series: [
        // uPlot types the raw value as a number; at a gap it is null.
        { label: "time", value: (_u: uPlot, v: number | null) => (v === null ? "—" : xLabel(v)) },
        ...spec.series.map((s) => ({
          label: s.label,
          stroke: stroke(s.role, s.role === "data" ? dataIndex++ : 0),
          width: s.role === "data" ? 2 : 1.5,
          ...(s.role === "data" ? {} : { dash: [6, 4] }),
          points: { show: false },
          value: (_u: uPlot, v: number | null) => valueLabel(spec.unit, v),
        })),
      ],
    };
    const u = new uPlot(options, toAligned(x, spec.series), element);
    const line = document.createElement("div");
    line.className = "chart-cursor";
    u.over.appendChild(line);
    u.over.addEventListener("click", () => {
      const idx = u.cursor.idx;
      if (idx != null) seek.current(idx);
    });
    plot.current = u;
    cursor.current = line;
    const resize = new ResizeObserver(() => {
      u.setSize({ width: element.clientWidth, height: HEIGHT });
    });
    resize.observe(element);
    return () => {
      resize.disconnect();
      u.destroy();
      plot.current = null;
      cursor.current = null;
    };
  }, [spec, x, xLabel]);

  useEffect(() => {
    const u = plot.current;
    const line = cursor.current;
    const at = x[frame];
    if (u === null || line === null || at === undefined) return;
    line.style.left = `${u.valToPos(at, "x")}px`;
  });

  return (
    <figure className="chart" aria-label={spec.title} title={spec.about}>
      <figcaption>
        {spec.title} <span className="unit">{spec.unit === "share" ? "%" : spec.unit}</span>
      </figcaption>
      <div ref={box} />
    </figure>
  );
}
