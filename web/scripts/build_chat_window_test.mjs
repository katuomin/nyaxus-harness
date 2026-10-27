import { build } from "esbuild";

await build({
  entryPoints: ["scripts/chat_window_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/chat_window_test.mjs",
  logLevel: "warning",
});

await import("../dist/chat_window_test.mjs");
