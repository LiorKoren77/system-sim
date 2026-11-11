// The page's panes: the list of scenarios on the left, which hides, and the charts panel on the
// right, whose border with the diagram is dragged. Everything here is a pure function of numbers
// (tested without a browser); `Splitter.tsx` and `App.tsx` feed it pointer positions and keys.

export const SIDE_DEFAULT = 420;
// Under this the charts' y axis (52 px) and legend leave too little plot to read.
export const SIDE_MIN = 280;
// What the diagram keeps however wide the charts are dragged: about two boxes side by side.
export const DIAGRAM_MIN = 320;
// One arrow key's move: small enough to place the border, large enough to cross the stage quickly.
const KEY_STEP = 24;

export interface Panes {
  catalogueOpen: boolean;
  sideWidth: number;
}

export const defaultPanes: Panes = { catalogueOpen: true, sideWidth: SIDE_DEFAULT };

// The widest the charts panel may be on a stage of this width. A stage not measured yet (0, as in
// jsdom, which lays nothing out) bounds nothing; one too narrow for both keeps the panel's minimum.
export function maxSideWidth(stageWidth: number): number {
  if (stageWidth <= 0) return Number.POSITIVE_INFINITY;
  return Math.max(SIDE_MIN, stageWidth - DIAGRAM_MIN);
}

export function clampSideWidth(width: number, stageWidth: number): number {
  return Math.min(Math.max(width, SIDE_MIN), maxSideWidth(stageWidth));
}

// The panel is right of the splitter, so a pointer that went left of where the drag began widens
// it by as much. Measured from the drag's start, not from the last event: no drift when the pointer
// overshoots a bound and comes back.
export function sideWidthAfterDrag(
  start: { width: number; x: number },
  x: number,
  stageWidth: number,
): number {
  return clampSideWidth(start.width + (start.x - x), stageWidth);
}

// The keys of a window splitter (WAI-ARIA): an arrow moves the border the way it points, Home and
// End take it to its ends. null for a key that is not the splitter's, which the page then handles.
export function sideWidthAfterKey(key: string, width: number, stageWidth: number): number | null {
  switch (key) {
    case "ArrowLeft":
      return clampSideWidth(width + KEY_STEP, stageWidth);
    case "ArrowRight":
      return clampSideWidth(width - KEY_STEP, stageWidth);
    case "Home":
      return clampSideWidth(Number.MAX_SAFE_INTEGER, stageWidth);
    case "End":
      return SIDE_MIN;
    default:
      return null;
  }
}

// ---- remembered per browser ----

const STORAGE_KEY = "simsys.panes";

// What was saved, each part checked on its own: the text comes from storage a user or an older
// version of the page may have written, so a part that is not what it should be takes its default.
export function parsePanes(text: string | null): Panes {
  if (text === null) return defaultPanes;
  let saved: unknown;
  try {
    saved = JSON.parse(text);
  } catch {
    return defaultPanes;
  }
  if (typeof saved !== "object" || saved === null) return defaultPanes;
  // `in` narrows `object` to one that has the key: TypeScript's way to read a key of unknown JSON.
  const open = "catalogueOpen" in saved ? saved.catalogueOpen : undefined;
  const width = "sideWidth" in saved ? saved.sideWidth : undefined;
  return {
    catalogueOpen: typeof open === "boolean" ? open : defaultPanes.catalogueOpen,
    sideWidth:
      typeof width === "number" && Number.isFinite(width)
        ? clampSideWidth(width, 0)
        : defaultPanes.sideWidth,
  };
}

// localStorage can throw (a private window, blocked site data): the page then opens with the
// defaults and forgets, which costs the viewer one click.
export function readPanes(): Panes {
  try {
    return parsePanes(window.localStorage.getItem(STORAGE_KEY));
  } catch {
    return defaultPanes;
  }
}

export function writePanes(panes: Panes): void {
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(panes));
  } catch {
    // Nothing to do: see readPanes.
  }
}
