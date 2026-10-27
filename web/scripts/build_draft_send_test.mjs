// Bundles the draft-send regression test (no shell quoting for the
// NODE_ENV define) and runs it.
import { build } from "esbuild";

await build({
  entryPoints: ["scripts/draft_send_test.ts"],
  bundle: true,
  format: "esm",
  jsx: "automatic",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  outfile: "dist/draft_send_test.mjs",
  logLevel: "warning",
});

await import("../dist/draft_send_test.mjs");
