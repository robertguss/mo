import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: true,
    origin: "http://127.0.0.1:5173",
  },
  build: {
    outDir: "../public",
    emptyOutDir: true,
    manifest: "manifest.json",
    assetsDir: "assets",
    rollupOptions: {
      input: "src/main.tsx",
    },
  },
});
