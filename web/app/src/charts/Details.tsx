import type { DatasetStore } from "@simsys/stream";
import { useMemo } from "react";
import { clockLabel } from "../timeline/clock";
import { Chart } from "./Chart";
import { chartsFor, xValues } from "./series";

interface Shown {
  readonly kind: "day" | "run";
  readonly window: { readonly from_s: number; readonly to_s: number };
}

// The charts of the selected box (or bandwidth link) over the whole window, each with a line at
// the slider's frame. The series are built once the dataset has loaded and again only when the
// selection changes: a chart is redrawn by uPlot, not by React, while the day plays.
export function Details(props: {
  store: DatasetStore;
  selected: string | null;
  frame: number;
  shown: Shown;
  onSeek: (frame: number) => void;
}) {
  const { store, selected, frame, shown, onSeek } = props;
  const graph = store.graph;
  const ready = store.status === "ready" ? store.version : -1;
  const from_s = shown.window.from_s;
  const x = useMemo(
    () => xValues(store, from_s),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- ready stands for the store's contents
    [store, from_s, ready],
  );
  const charts = useMemo(
    () => (graph && selected !== null && ready >= 0 ? chartsFor(graph, store, selected) : []),
    [graph, store, selected, ready],
  );
  const kind = shown.kind;
  const to_s = shown.window.to_s;
  const xLabel = useMemo(() => {
    const window = { from_s, to_s };
    return (v: number) => clockLabel(v + from_s, { kind, window });
  }, [kind, from_s, to_s]);

  const node = graph?.nodes.find((n) => n.id === selected);
  const link = graph?.links.find((l) => `${l.from}->${l.to}` === selected);
  // Nothing chosen, or an address naming what this scenario does not have.
  if (selected === null || (node === undefined && link === undefined)) {
    return <p className="hint">Click a box to chart its day.</p>;
  }
  return (
    <section className="details" aria-label="Charts">
      <h2>{link ? `${link.from} → ${link.to}` : selected}</h2>
      {node && <p className="type">{node.type.replaceAll("_", " ")}</p>}
      {link && (
        <p className="type">
          link of {link.bandwidth_mb_s ?? "unlimited"} MB/s, {link.one_way_s * 1000} ms one way
        </p>
      )}
      {charts.map((spec) => (
        <Chart
          key={`${selected}-${spec.key}`}
          spec={spec}
          x={x}
          frame={frame}
          xLabel={xLabel}
          onSeek={onSeek}
        />
      ))}
    </section>
  );
}
