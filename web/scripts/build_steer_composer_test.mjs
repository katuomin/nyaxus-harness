import { build } from "esbuild";

await build({
  entryPoints: ["scripts/steer_composer_test.ts"],
  bundle: true,
  platform: "node",
  format: "esm",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  outfile: "dist/steer_composer_test.mjs",
  logLevel: "warning",
});

await import("../dist/steer_composer_test.mjs");
