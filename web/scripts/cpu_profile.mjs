// Capture a 30s JS CPU profile of the nyaxus page via CDP and summarize the
// top functions by self-time. Attach to a running browser on the debug port.
//
//   node scripts/cpu_profile.mjs [seconds]   (default 30)
import fs from "node:fs";
import path from "node:path";
import puppeteer from "puppeteer-core";

const SECONDS = Number(process.argv[2] || 30);
const browser = await puppeteer.connect({ browserURL: process.env.BROWSER_URL || "http://127.0.0.1:9222" });
const pages = await browser.pages();
const page = pages.find((p) => p.url().includes("4173"));
if (!page) {
  console.log("no nyaxus page found");
  process.exit(1);
}
const cdp = await page.createCDPSession();
await cdp.send("Profiler.enable");
await cdp.send("Profiler.start");
console.log(`[cpu_profile] profiling ${SECONDS}s...`);
await new Promise((r) => setTimeout(r, SECONDS * 1000));
const { profile } = await cdp.send("Profiler.stop");
await cdp.send("Profiler.disable");

// CDP profile is sample-based: samples[i] is a node id, timeDeltas[i] the
// ms elapsed since the previous sample. Self-time per node = sum of the
// deltas of samples that point directly at it.
const byFn = new Map();
const totalUs = profile.timeDeltas.reduce((a, b) => a + b, 0) * 1000;
profile.samples.forEach((nodeId, i) => {
  const node = profile.nodes[nodeId];
  if (!node) return;
  const frame = node.callFrame;
  const name = frame.functionName || "(anonymous)";
  const url = (frame.url || "").split("/").slice(-2).join("/");
  const key = `${name} ${url}:${frame.lineNumber}`;
  byFn.set(key, (byFn.get(key) || 0) + profile.timeDeltas[i] * 1000);
});

const rows = [...byFn.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25);
const totalSec = totalUs / 1e6;
console.log(`[cpu_profile] total JS self-time ${totalSec.toFixed(2)}s over ${SECONDS}s (${(100 * totalSec / SECONDS).toFixed(1)}% of one core)`);
for (const [key, us] of rows) {
  console.log(`  ${(100 * us / totalUs).toFixed(1).padStart(5)}%  ${(us / 1e6).toFixed(3)}s  ${key.slice(0, 110)}`);
}
fs.writeFileSync(path.join(OUT_DIR, "cpu-profile.json"), JSON.stringify(profile));
const OUT_DIR = process.env.OUT_DIR || "C:/Users/<user>/AppData/Local/Temp/nyaxus-profile";
fs.mkdirSync(OUT_DIR, { recursive: true });
await browser.disconnect();
