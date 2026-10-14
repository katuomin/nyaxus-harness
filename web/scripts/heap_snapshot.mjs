// Capture a full CDP heap snapshot from the nyaxus page (Chrome 154+ streams
// the snapshot in addHeapSnapshotChunk events) and save it as a standard
// .heapsnapshot JSON file for scripts/heap_diff.mjs.
//
//   node scripts/heap_snapshot.mjs [out.heapsnapshot]
import fs from "node:fs";
import path from "node:path";
import puppeteer from "puppeteer-core";

const out = process.argv[2]
  ? path.resolve(process.argv[2])
  : path.resolve(new URL("..", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1"), "profile", `heap-${Date.now()}.heapsnapshot`);

const browser = await puppeteer.connect({ browserURL: process.env.BROWSER_URL || "http://127.0.0.1:9222" });
const pages = await browser.pages();
const page = pages.find((p) => p.url().includes("4173"));
if (!page) {
  console.log("no nyaxus page found");
  process.exit(1);
}
const cdp = await page.createCDPSession();

const chunks = [];
cdp.on("HeapProfiler.addHeapSnapshotChunk", (params) => chunks.push(params.chunk));
// Resolves when the snapshot is complete; all chunks have arrived by then.
await cdp.send("HeapProfiler.takeHeapSnapshot", { reportProgress: false });

const text = chunks.join("");
fs.writeFileSync(out, text);
console.log(`[heap_snapshot] ${out} (${(fs.statSync(out).size / 1048576).toFixed(1)} MiB)`);
await cdp.detach();
await browser.disconnect();
