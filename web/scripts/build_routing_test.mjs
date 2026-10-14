import { build } from "esbuild";

await build({
  entryPoints: ["scripts/routing_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/routing_test.mjs",
  logLevel: "warning",
});

await import("../dist/routing_test.mjs");
