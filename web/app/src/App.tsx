import { FileSource, type DatasetSource, type DatasetStore } from "@simsys/stream";
import { useEffect, useMemo, useReducer, useState } from "react";
import { Catalogue } from "./catalogue/Catalogue";
import { defaultEntry, fetchIndex, type Index, type IndexEntry } from "./catalogue/index-file";
import { Diagram } from "./diagram/Diagram";
import { readHash, writeHash } from "./hash";
import {
  createPlayer,
  playerReducer,
  type PlayerAction,
  type PlayerState,
} from "./timeline/player";
import { Timeline } from "./timeline/Timeline";
import { useDataset } from "./use-dataset";

// The page: the catalogue, and the chosen dataset's diagram and time bar. Where the data comes from
// is injected (`loadIndex`, `sourceFor`), so the tests give it fixtures and the page static files.
export function App(props: {
  loadIndex?: () => Promise<Index>;
  sourceFor?: (entry: IndexEntry) => DatasetSource;
}) {
  const [index, setIndex] = useState<Index | null>(null);
  const [indexFailed, setIndexFailed] = useState(false);
  const [selected, setSelected] = useState<string | null>(() => readHash().id);
  const [node, setNode] = useState<string | null>(() => readHash().node);
  // The injected functions are taken once: a new function per render must not reload anything.
  const [loadIndex] = useState(() => props.loadIndex ?? (() => fetchIndex()));
  const [sourceFor] = useState(
    () => props.sourceFor ?? ((e: IndexEntry) => new FileSource(`datasets/${e.file}`)),
  );

  useEffect(() => {
    loadIndex().then(setIndex, () => {
      setIndexFailed(true);
    });
  }, [loadIndex]);

  const entry =
    index?.datasets.find((d) => d.id === selected) ?? (index ? defaultEntry(index) : undefined);
  const source = useMemo(() => (entry ? sourceFor(entry) : null), [entry, sourceFor]);
  const store = useDataset(source);

  const choose = (id: string) => {
    setSelected(id);
    setNode(null);
    writeHash(id, null);
  };
  const chooseNode = (id: string | null) => {
    setNode(id);
    if (entry) writeHash(entry.id, id);
  };

  if (indexFailed) return <p className="notice">No datasets yet: run npm run precompute in web/</p>;
  if (index === null) return <p className="notice">Loading the catalogue…</p>;
  return (
    <div className="app">
      <Catalogue index={index} selected={entry?.id ?? null} onSelect={choose} />
      <main>
        {entry && (
          <h1>
            {entry.title ?? entry.id}
            {entry.chapter !== null && <span className="chapter"> · ch. {entry.chapter}</span>}
          </h1>
        )}
        {store && <Dataset key={entry?.id} store={store} node={node} onSelectNode={chooseNode} />}
      </main>
    </div>
  );
}

// One dataset on the page: its status while it loads or when it failed, then the diagram and the
// time bar. Keyed by the dataset's id, so a new choice starts at frame 0.
function Dataset(props: {
  store: DatasetStore;
  node: string | null;
  onSelectNode: (id: string | null) => void;
}) {
  const { store } = props;
  const dataset = store.dataset;
  const shown = {
    kind: dataset?.kind ?? "run",
    window: dataset?.window ?? { from_s: 0, to_s: store.run?.duration_s ?? 0 },
  };
  // The store fills while the file downloads: the player's frames follow it. A reducer declared in
  // the component sees this render's values, so each action is applied to the frames there are now.
  const config = {
    frames: store.frameCount,
    loops: dataset?.loops ?? false,
    framesPerHour: store.run ? 3600 / store.run.bucket_s : 1,
  };
  const [player, dispatch] = useReducer(
    (state: PlayerState, action: PlayerAction) => playerReducer({ ...state, ...config }, action),
    createPlayer(config),
  );
  const effective = { ...player, ...config };
  usePlayback(effective.playing, dispatch);
  useKeys(dispatch);

  if (store.status === "error") return <Problem store={store} />;
  if (store.status === "truncated") {
    return <p className="notice">The dataset ended early: rebuild it with npm run precompute</p>;
  }
  if (store.frameCount === 0) return <p className="notice">Loading…</p>;
  return (
    <>
      <Diagram
        store={store}
        frame={Math.min(effective.frame, store.frameCount - 1)}
        selected={props.node}
        onSelect={props.onSelectNode}
        playing={effective.playing}
      />
      <Timeline store={store} player={effective} dispatch={dispatch} shown={shown} />
    </>
  );
}

function Problem({ store }: { store: DatasetStore }) {
  const p = store.problem;
  const where = p?.file ? `${p.file}${p.line ? `:${p.line}:${p.column ?? 0}` : ""}: ` : "";
  return (
    <p className="notice error">{`${where}${p?.message ?? "the dataset could not be read"}`}</p>
  );
}

// While playing, one tick per animation frame with the time since the last one.
function usePlayback(playing: boolean, dispatch: (action: PlayerAction) => void) {
  useEffect(() => {
    if (!playing) return;
    let last = performance.now();
    let id = requestAnimationFrame(function step(now) {
      dispatch({ type: "tick", seconds: Math.min((now - last) / 1000, 0.25) });
      last = now;
      id = requestAnimationFrame(step);
    });
    return () => {
      cancelAnimationFrame(id);
    };
  }, [playing, dispatch]);
}

// Space plays and pauses, ← → move a frame, with Shift an hour, Home goes to the start.
function useKeys(dispatch: (action: PlayerAction) => void) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.target instanceof HTMLSelectElement || e.target instanceof HTMLTextAreaElement) return;
      const action = keyAction(e);
      if (action === null) return;
      e.preventDefault();
      dispatch(action);
    };
    window.addEventListener("keydown", onKey);
    return () => {
      window.removeEventListener("keydown", onKey);
    };
  }, [dispatch]);
}

function keyAction(e: KeyboardEvent): PlayerAction | null {
  switch (e.key) {
    case " ":
      return { type: "toggle" };
    case "ArrowRight":
      return e.shiftKey ? { type: "stepHour", by: 1 } : { type: "step", by: 1 };
    case "ArrowLeft":
      return e.shiftKey ? { type: "stepHour", by: -1 } : { type: "step", by: -1 };
    case "Home":
      return { type: "seek", frame: 0 };
    default:
      return null;
  }
}
