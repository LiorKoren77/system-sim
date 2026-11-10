import react from "@vitejs/plugin-react";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [react()],
  test: {
    name: "app",
    root: import.meta.dirname,
    environment: "jsdom",
    setupFiles: ["./test/setup.ts"],
  },
});
