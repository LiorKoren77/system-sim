import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// The page is static files: `vite build` writes them to dist/, public/datasets (written by
// `npm run precompute`) copied in, and `vite preview` serves them on localhost:4173.
export default defineConfig({
  plugins: [react()],
  base: "./",
});
