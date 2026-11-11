import { durationLabel } from "../timeline/clock";
import type { Index, IndexEntry } from "./index-file";

// The list of datasets, grouped as scenarios/ is (book, labs, tutorial), each with a badge that says
// how it plays: a repeating day loops; a day that does not repeat plays both of its days once; a
// run plays once.
export function Catalogue(props: {
  index: Index;
  selected: string | null;
  onSelect: (id: string) => void;
}) {
  const groups = byGroup(props.index.datasets);
  return (
    <nav className="catalogue" aria-label="Scenarios">
      {[...groups].map(([group, entries]) => (
        <section key={group}>
          <h2>{group}</h2>
          <ul>
            {entries.map((entry) => (
              <li key={entry.id}>
                <button
                  type="button"
                  aria-current={entry.id === props.selected ? "true" : undefined}
                  onClick={() => {
                    props.onSelect(entry.id);
                  }}
                >
                  <span className="title">{entry.title ?? entry.id}</span>
                  {entry.chapter !== null && <span className="chapter">ch. {entry.chapter}</span>}
                  <Badge entry={entry} />
                </button>
              </li>
            ))}
          </ul>
        </section>
      ))}
    </nav>
  );
}

function Badge({ entry }: { entry: IndexEntry }) {
  if (entry.kind === "run")
    return <span className="badge run">run · {durationLabel(entry.duration_s)}</span>;
  if (entry.loops) return <span className="badge loops">repeating day</span>;
  return <span className="badge drifts">does not repeat</span>;
}

// The datasets by the folder their id starts with, in the index's order (sorted by id).
function byGroup(datasets: readonly IndexEntry[]): Map<string, IndexEntry[]> {
  const groups = new Map<string, IndexEntry[]>();
  for (const entry of datasets) {
    const group = entry.id.split("/")[0] ?? "";
    groups.set(group, [...(groups.get(group) ?? []), entry]);
  }
  return groups;
}
