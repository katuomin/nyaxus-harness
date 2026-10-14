import { build } from "esbuild";

await build({
  entryPoints: ["scripts/markdown_render_test.tsx"],
  bundle: true,
  platform: "node",
  format: "esm",
  jsx: "automatic",
  define: { "process.env.NODE_ENV": JSON.stringify("production") },
  external: ["react", "react/*", "react-dom", "react-dom/*"],
  // KaTeX's CSS references font files; the test bundle only needs the markup,
  // so emit the fonts as files instead of inlining them.
  loader: { ".woff": "file", ".woff2": "file", ".eot": "file", ".ttf": "file", ".svg": "file" },
  outfile: "dist/markdown_render_test.mjs",
  logLevel: "warning",
});

await import("../dist/markdown_render_test.mjs");
