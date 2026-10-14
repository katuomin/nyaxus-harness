import { build } from "esbuild";

await build({
  entryPoints: ["scripts/skills_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/skills_test.mjs",
  logLevel: "warning",
});

await import("../dist/skills_test.mjs");
