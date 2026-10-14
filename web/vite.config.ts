import type { Server as HttpServer } from "node:http";
import path from "path";
import react from "@vitejs/plugin-react";
import { defineConfig, type Plugin } from "vite";

// Node's default (5s) keepAliveTimeout closes idle pooled sockets while the
// browser still holds them, so the next EventSource lands on a dead socket
// and waits its ~5s default reconnect before the session switch completes.
// Keep pooled sockets alive across session switches.
const keepAliveLonger: Plugin = {
  name: "bump-keep-alive-timeout",
  configureServer(server) {
    // Post-hook: runs after the dev server (and its http server) is created.
    return () => {
      if (server.httpServer) {
        (server.httpServer as unknown as HttpServer).keepAliveTimeout = 65_000;
      }
    };
  },
};

const proxyRoutes = {
  "/api": process.env.NYAXUS_PROXY || "http://localhost:8787",
  "/auth": process.env.NYAXUS_PROXY || "http://localhost:8787",
};

export default defineConfig({
  base: "./",
  plugins: [react(), keepAliveLonger],
  server: {
    host: "0.0.0.0",
    port: 4173,
    // The /api prefix is the single route inventory's proxy boundary:
    // harness/tests/api_contract.json (checked by harness/tests/api_parity.mjs)
    // lists every route under it, so a new daemon route is covered by the
    // proxy automatically.
    proxy: proxyRoutes,
  },
  // `vite preview` serves the prod bundle from dist/ with the same proxy so a
  // production-build smoke test behaves like the dev server.
  preview: {
    host: "0.0.0.0",
    port: 4173,
    proxy: proxyRoutes,
  },
  build: {
    outDir: "dist",
    emptyOutDir: true,
  },
  resolve: {
    alias: { "@": path.resolve(__dirname, "./src") },
  },
});
