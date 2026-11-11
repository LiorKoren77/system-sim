import { defineConfig } from "vitest/config";

// One test run over the three workspaces, each in the environment it runs in: the stream package
// and the precompute in Node, the app in a simulated browser (jsdom).
export default defineConfig({
  test: {
    projects: [
      { test: { name: "stream", root: "stream", environment: "node" } },
      { test: { name: "precompute", root: "precompute", environment: "node" } },
      "app/vitest.config.ts",
    ],
  },
});
