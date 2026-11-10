import type { DatasetStore } from "@simsys/stream";

// The system's diagram: Task 6 of the 5b plan.
export function Diagram(props: {
  store: DatasetStore;
  frame: number;
  selected: string | null;
  onSelect: (id: string | null) => void;
}) {
  return <div className="diagram" data-frame={props.frame} />;
}
