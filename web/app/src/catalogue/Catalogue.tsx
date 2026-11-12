import { durationLabel } from "../timeline/clock";
import type { Index, IndexEntry } from "./index-file";

// The list of datasets, grouped as scenarios/ is (book, labs, tutorial), each with a badge that says
// how it plays: a repeating day loops; a day that does not repeat plays both of its days once; a
// run plays once. The list hides to a narrow rail with the button that brings it back, which gives
// its 260 px to the diagram.
export function Catalogue(props: {
  index: Index;
  selected: string | null;
  onSelect: (id: string) => void;
  open: boolean;
  onToggle: () => void;
}) {
  if (!props.open) {
    return (
      <nav className="catalogue collapsed" aria-label="Scenarios">
        <PaneToggle open={false} onToggle={props.onToggle} />
      </nav>
    );
  }
  const groups = byGroup(props.index.datasets);
  return (
    <nav className="catalogue" aria-label="Scenarios">
      <PaneToggle open onToggle={props.onToggle} />
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

// The chevron points the way the list will go; the label says it in words (aria-expanded tells a
// screen reader which state the list is in).
function PaneToggle(props: { open: boolean; onToggle: () => void }) {
  const label = props.open ? "Hide the scenarios" : "Show the scenarios";
  return (
    <button
      type="button"
      className="pane-toggle"
      aria-label={label}
      aria-expanded={props.open}
      title={label}
      onClick={props.onToggle}
    >
      {props.open ? "«" : "»"}
    </button>
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
