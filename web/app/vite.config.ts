import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// The page is static files: `vite build` writes them to dist/, public/datasets (written by
// `npm run precompute`) copied in, and `vite preview` serves them on localhost:4173.
export default defineConfig({
  plugins: [react()],
  base: "./",
  // ELK's layout engine is one 1.5 MB file (452 kB gzipped), loaded on first use (diagram/layout.ts):
  // large, but split off and expected.
  build: { chunkSizeWarningLimit: 1600 },
});
