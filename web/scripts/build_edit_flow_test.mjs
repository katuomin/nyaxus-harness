// Bundles the edit-flow regression test (no shell quoting for the
// NODE_ENV define) and runs it.
import { build } from "esbuild";

await build({
  entryPoints: ["scripts/edit_flow_test.ts"],
  bundle: true,
  format: "esm",
  jsx: "automatic",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  outfile: "dist/edit_flow_test.mjs",
  logLevel: "warning",
});

await import("../dist/edit_flow_test.mjs");
