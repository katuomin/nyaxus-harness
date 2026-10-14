import { build } from "esbuild";

await build({
  entryPoints: ["scripts/reasoning_blocks_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/reasoning_blocks_test.mjs",
  logLevel: "warning",
});

await import("../dist/reasoning_blocks_test.mjs");
