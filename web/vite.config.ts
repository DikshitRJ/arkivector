import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

// Clean URLs need an absolute base. Set VITE_BASE=/arkivector/ when deploying under a sub-path
// (for example a GitHub Pages project site).
export default defineConfig({
  base: process.env.VITE_BASE ?? "/",
  plugins: [react(), tailwindcss()],
  // /docs imports the Markdown guides from ../docs/guide (shared with the repository and the wiki)
  server: { fs: { allow: [".."] } },
});
