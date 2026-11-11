import type { DatasetStore } from "@simsys/stream";
import { sloRows } from "./slo-rows";

// The scenario's objectives, judged over the observed window (a day's: day 2, the day the page
// plays), as `simsys run` prints them: what, of what, the bound, what was measured, the verdict.
export function SloTable({ store }: { store: DatasetStore }) {
  const report = store.summary?.report;
  if (!report || !store.graph || !store.dataset) return null;
  const rows = sloRows(report, store.graph, store.dataset);
  if (rows.length === 0) return <p className="hint">The scenario states no SLO.</p>;
  return (
    <table className="slo" aria-label="SLOs">
      <thead>
        <tr>
          <th>SLO</th>
          <th>of</th>
          <th>bound</th>
          <th>measured</th>
          <th>verdict</th>
        </tr>
      </thead>
      <tbody>
        {rows.map((row, i) => (
          <tr key={i} className={row.status}>
            <td>{row.metric}</td>
            <td>{row.target}</td>
            <td>{row.bound}</td>
            <td>
              {row.observed}
              {row.window !== null && <small>{row.window}</small>}
              {row.note !== null && <small>{row.note}</small>}
            </td>
            <td>
              <span className={`verdict ${row.status}`}>{row.status}</span>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
