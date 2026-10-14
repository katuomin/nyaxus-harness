import { build } from "esbuild";

await build({
  entryPoints: ["scripts/tasks_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/tasks_test.mjs",
  logLevel: "warning",
});

await import("../dist/tasks_test.mjs");
