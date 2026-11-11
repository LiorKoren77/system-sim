import "@testing-library/jest-dom/vitest";
import { cleanup } from "@testing-library/react";
import { afterEach } from "vitest";

// Testing Library unmounts what a test rendered only when the runner's globals are on; they are
// off here (explicit imports), so it is done by hand.
afterEach(() => {
  cleanup();
});

// jsdom has no canvas (it would need a native package): getContext answers null, as a browser does
// for a context it cannot make, and the zone strip then draws nothing. Without this stub jsdom
// prints "Not implemented" for each strip rendered.
HTMLCanvasElement.prototype.getContext = () => null;
