// The time model of the page, as a pure reducer: (state, action) → state, so looping, stopping
// and stepping are tested without timers. The component drives `tick` from the browser's
// animation frames (Timeline.tsx). Pattern: State machine — playing or paused, and where.

export interface PlayerState {
  readonly frame: number;
  readonly frames: number;
  readonly loops: boolean; // a day that repeats wraps from its last frame to its first
  readonly framesPerHour: number;
  readonly seconds: number; // how long the whole window takes to play
  readonly playing: boolean;
  readonly carry: number; // the part of a frame the ticks so far have not yet made a whole frame
}

export type PlayerAction =
  | { readonly type: "play" }
  | { readonly type: "pause" }
  | { readonly type: "toggle" }
  | { readonly type: "tick"; readonly seconds: number }
  | { readonly type: "seek"; readonly frame: number }
  | { readonly type: "step"; readonly by: number }
  | { readonly type: "stepHour"; readonly by: number }
  | { readonly type: "setSpeed"; readonly seconds: number };

// A speed is how long the whole window takes to play, whatever its frames: a day of 1440 frames of
// 1 min in a minute is 24 frames a second, a run of 720 frames in a minute 12. Chosen, not from the
// book: a minute is long enough to see a wave rise and short enough to watch a whole day.
export const SPEEDS = [
  { label: "30 s", seconds: 30 },
  { label: "1 min", seconds: 60 },
  { label: "2 min", seconds: 120 },
  { label: "5 min", seconds: 300 },
] as const;

export function createPlayer(options: {
  frames: number;
  loops: boolean;
  framesPerHour: number;
}): PlayerState {
  return { ...options, frame: 0, seconds: 60, playing: false, carry: 0 };
}

export function playerReducer(state: PlayerState, action: PlayerAction): PlayerState {
  switch (action.type) {
    case "play":
      // Play at the end of a day that does not repeat, or of a run: from the start again.
      return { ...state, playing: true, frame: atEnd(state) && !state.loops ? 0 : state.frame };
    case "pause":
      return { ...state, playing: false, carry: 0 };
    case "toggle":
      return playerReducer(state, { type: state.playing ? "pause" : "play" });
    case "tick":
      return state.playing ? advance(state, action.seconds) : state;
    case "seek":
      return { ...state, frame: clamp(Math.round(action.frame), state), carry: 0 };
    case "step":
      return { ...state, frame: moveBy(state, action.by), carry: 0 };
    case "stepHour":
      return { ...state, frame: moveBy(state, action.by * state.framesPerHour), carry: 0 };
    case "setSpeed":
      return { ...state, seconds: action.seconds };
  }
}

function advance(state: PlayerState, seconds: number): PlayerState {
  const exact = state.carry + (seconds * state.frames) / state.seconds;
  // A small epsilon: 12 ticks of 1/288 s at 24 frames a second must make exactly one frame, not
  // 0.9999999999999999 of one.
  const whole = Math.floor(exact + 1e-9);
  const carry = Math.max(0, exact - whole);
  const target = state.frame + whole;
  if (target < state.frames) return { ...state, frame: target, carry };
  if (state.loops) return { ...state, frame: target % state.frames, carry };
  return { ...state, frame: state.frames - 1, playing: false, carry: 0 };
}

function moveBy(state: PlayerState, by: number): number {
  const target = state.frame + Math.round(by);
  if (state.loops) return ((target % state.frames) + state.frames) % state.frames;
  return clamp(target, state);
}

function clamp(frame: number, state: PlayerState): number {
  return Math.min(Math.max(frame, 0), Math.max(state.frames - 1, 0));
}

function atEnd(state: PlayerState): boolean {
  return state.frame >= state.frames - 1;
}
