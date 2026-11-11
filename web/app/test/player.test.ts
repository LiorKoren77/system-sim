import { describe, expect, it } from "vitest";
import {
  createPlayer,
  playerConfig,
  playerReducer,
  SPEEDS,
  type PlayerState,
} from "../src/timeline/player";

// A day: 1440 frames of 1 min, 60 to the hour.
function day(frame: number, loops = true): PlayerState {
  return { ...createPlayer({ frames: 1440, loops, framesPerHour: 60 }), frame };
}

const play = (s: PlayerState) => playerReducer(s, { type: "play" });
const tick = (s: PlayerState, seconds: number) => playerReducer(s, { type: "tick", seconds });

describe("the player", () => {
  it("plays a day in a minute at the default speed: 24 frames a second", () => {
    expect(tick(play(day(0)), 1).frame).toBe(24);
    expect(tick(play(day(0)), 0.5).frame).toBe(12);
  });

  it("plays a run of 720 frames in a minute too: the speed is the whole window's length", () => {
    const run = { ...createPlayer({ frames: 720, loops: false, framesPerHour: 1 }), playing: true };
    expect(tick(run, 1).frame).toBe(12);
  });

  it("carries a fraction of a frame to the next tick, so slow ticks still add up", () => {
    let s = play(day(0));
    for (let i = 0; i < 12; ++i) s = tick(s, 1 / 288); // 12 ticks of a twelfth of a frame
    expect(s.frame).toBe(1);
  });

  it("wraps a looping day from 23:59 to 00:00 and keeps playing", () => {
    const s = tick(play(day(1439)), 1 / 24);
    expect(s.frame).toBe(0);
    expect(s.playing).toBe(true);
  });

  it("stops at the end of a day that does not repeat, and plays again from the start", () => {
    const s = tick({ ...day(1439, false), playing: true }, 1);
    expect(s.frame).toBe(1439);
    expect(s.playing).toBe(false);
    expect(play(s).frame).toBe(0);
  });

  it("steps by an hour, wrapping in a looping day and stopping at the end otherwise", () => {
    expect(playerReducer(day(1400), { type: "stepHour", by: 1 }).frame).toBe(20);
    expect(playerReducer(day(1400, false), { type: "stepHour", by: 1 }).frame).toBe(1439);
    expect(playerReducer(day(5), { type: "stepHour", by: -1 }).frame).toBe(1385);
  });

  it("steps by a frame and seeks within the frames", () => {
    expect(playerReducer(day(0), { type: "step", by: -1 }).frame).toBe(1439);
    expect(playerReducer(day(0, false), { type: "step", by: -1 }).frame).toBe(0);
    expect(playerReducer(day(0), { type: "seek", frame: 2000 }).frame).toBe(1439);
  });

  it("offers a day in 30 s, 1 min, 2 min and 5 min", () => {
    expect(SPEEDS.map((s) => s.seconds)).toEqual([30, 60, 120, 300]);
    const fast = playerReducer(play(day(0)), { type: "setSpeed", seconds: 30 });
    expect(tick(fast, 1).frame).toBe(48);
  });
});

describe("the player's frames, from the store as it fills", () => {
  const looping = { loops: true, window: { from_s: 0, to_s: 86400 } } as const;

  it("wraps a looping day only once the whole day has arrived", () => {
    const loading = { frameCount: 300, status: "loading", dataset: looping, run: { bucket_s: 60 } };
    expect(playerConfig(loading)).toEqual({ frames: 300, loops: false, framesPerHour: 60 });
    expect(playerConfig({ ...loading, frameCount: 1440, status: "ready" }).loops).toBe(true);
  });

  it("does not wrap a run, which plays once", () => {
    const run = { frameCount: 720, status: "ready", dataset: undefined, run: { bucket_s: 5 } };
    expect(playerConfig(run)).toEqual({ frames: 720, loops: false, framesPerHour: 720 });
  });
});
