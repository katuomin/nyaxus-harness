import { build } from "esbuild";

await build({
  entryPoints: ["scripts/prompt_snippets_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/prompt_snippets_test.mjs",
  logLevel: "warning",
});

await import("../dist/prompt_snippets_test.mjs");
