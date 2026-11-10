import "@testing-library/jest-dom/vitest";
import { cleanup } from "@testing-library/react";
import { afterEach } from "vitest";

// Testing Library unmounts what a test rendered only when the runner's globals are on; they are
// off here (explicit imports), so it is done by hand.
afterEach(() => {
  cleanup();
});
