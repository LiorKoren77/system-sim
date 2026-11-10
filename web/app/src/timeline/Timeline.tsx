import type { DatasetStore } from "@simsys/stream";
import type { Dispatch } from "react";
import { clockLabel } from "./clock";
import { timelineMarks } from "./marks";
import { SPEEDS, type PlayerAction, type PlayerState } from "./player";

// The time bar: play and pause, the clock, the slider over the frames, the speed, and a mark for
// each failure and surge of load in the window (marks.ts; its length as a band), so the eye knows
// where to look before it happens.
export function Timeline(props: {
  store: DatasetStore;
  player: PlayerState;
  dispatch: Dispatch<PlayerAction>;
  shown: { kind: "day" | "run"; window: { from_s: number; to_s: number } };
}) {
  const { store, player, dispatch, shown } = props;
  const t = store.times()[player.frame] ?? shown.window.from_s;
  const span = shown.window.to_s - shown.window.from_s;
  const marks = store.graph ? timelineMarks(store.graph, shown.window) : [];
  return (
    <div className="timeline">
      <button
        type="button"
        className="play"
        aria-label={player.playing ? "Pause" : "Play"}
        onClick={() => {
          dispatch({ type: "toggle" });
        }}
      >
        {player.playing ? "❚❚" : "▶"}
      </button>
      <output className="clock" aria-label="Time">
        {clockLabel(t, shown)}
      </output>
      <div className="track">
        <input
          type="range"
          aria-label="Frame"
          min={0}
          max={Math.max(player.frames - 1, 0)}
          value={player.frame}
          onChange={(e) => {
            dispatch({ type: "seek", frame: Number(e.target.value) });
          }}
        />
        <div className="marks" aria-hidden="true">
          {marks.map((m) => (
            <span
              key={`${m.kind}-${m.label}-${m.at_s}`}
              className={`mark ${m.kind}`}
              title={m.label}
              style={{
                left: `${((m.at_s - shown.window.from_s) / span) * 100}%`,
                width: `max(4px, ${((m.for_s / span) * 100).toFixed(3)}%)`,
              }}
            />
          ))}
        </div>
      </div>
      <label className="speed">
        {shown.kind === "day" ? "day in" : "speed"}
        <select
          value={player.framesPerSecond}
          onChange={(e) => {
            dispatch({ type: "setSpeed", framesPerSecond: Number(e.target.value) });
          }}
        >
          {SPEEDS.map((s) => (
            <option key={s.label} value={s.framesPerSecond}>
              {shown.kind === "day" ? s.label : `${s.framesPerSecond} frames/s`}
            </option>
          ))}
        </select>
      </label>
    </div>
  );
}
