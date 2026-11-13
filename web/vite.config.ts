import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { siteFiles } from "./plugins/site-files";

// Clean URLs need an absolute base. Set VITE_BASE=/arkivector/ when deploying under a sub-path
// (for example a GitHub Pages project site).
export default defineConfig({
  base: process.env.VITE_BASE ?? "/",
  plugins: [react(), tailwindcss(), siteFiles()],
  // /docs renders the Markdown in ../docs/wiki (the same files are published to the GitHub wiki)
  server: { fs: { allow: [".."] } },
});
