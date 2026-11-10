import { describe, expect, it } from "vitest";
import { createPlayer, playerReducer, SPEEDS, type PlayerState } from "../src/timeline/player";

// A day: 720 frames of 2 min, 30 to the hour.
function day(frame: number, loops = true): PlayerState {
  return { ...createPlayer({ frames: 720, loops, framesPerHour: 30 }), frame };
}

const play = (s: PlayerState) => playerReducer(s, { type: "play" });
const tick = (s: PlayerState, seconds: number) => playerReducer(s, { type: "tick", seconds });

describe("the player", () => {
  it("plays a day in a minute at the default speed: 12 frames a second", () => {
    expect(tick(play(day(0)), 1).frame).toBe(12);
    expect(tick(play(day(0)), 0.5).frame).toBe(6);
  });

  it("carries a fraction of a frame to the next tick, so slow ticks still add up", () => {
    let s = play(day(0));
    for (let i = 0; i < 12; ++i) s = tick(s, 1 / 144); // 12 ticks of a twelfth of a frame
    expect(s.frame).toBe(1);
  });

  it("wraps a looping day from 23:58 to 00:00 and keeps playing", () => {
    const s = tick(play(day(719)), 1 / 12);
    expect(s.frame).toBe(0);
    expect(s.playing).toBe(true);
  });

  it("stops at the end of a day that does not repeat, and plays again from the start", () => {
    const s = tick({ ...day(719, false), playing: true }, 1);
    expect(s.frame).toBe(719);
    expect(s.playing).toBe(false);
    expect(play(s).frame).toBe(0);
  });

  it("steps by an hour, wrapping in a looping day and stopping at the end otherwise", () => {
    expect(playerReducer(day(700), { type: "stepHour", by: 1 }).frame).toBe(10);
    expect(playerReducer(day(700, false), { type: "stepHour", by: 1 }).frame).toBe(719);
    expect(playerReducer(day(5), { type: "stepHour", by: -1 }).frame).toBe(695);
  });

  it("steps by a frame and seeks within the frames", () => {
    expect(playerReducer(day(0), { type: "step", by: -1 }).frame).toBe(719);
    expect(playerReducer(day(0, false), { type: "step", by: -1 }).frame).toBe(0);
    expect(playerReducer(day(0), { type: "seek", frame: 900 }).frame).toBe(719);
  });

  it("offers a day in 30 s, 1 min, 2 min and 5 min", () => {
    expect(SPEEDS.map((s) => s.framesPerSecond)).toEqual([24, 12, 6, 2.4]);
    const fast = playerReducer(play(day(0)), { type: "setSpeed", framesPerSecond: 24 });
    expect(tick(fast, 1).frame).toBe(24);
  });
});
