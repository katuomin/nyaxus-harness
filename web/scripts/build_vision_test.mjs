// Bundles the vision-resolution regression test and runs it.
import { build } from "esbuild";

await build({
  entryPoints: ["scripts/vision_test.ts"],
  bundle: true,
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  outfile: "dist/vision_test.mjs",
  logLevel: "warning",
});

await import("../dist/vision_test.mjs");
