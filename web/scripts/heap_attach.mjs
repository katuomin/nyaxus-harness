// Attach to an already-running Chromium (Helium) and profile the nyaxus UI
// page while the user works in it normally.
//
// Launch the browser first (a fresh window with a debug port):
//   C:\Users\<user>\AppData\Local\imput\Helium\Application\chrome.exe
//     --remote-debugging-port=9222 --remote-allow-origins=*
// open http://localhost:4173/?heapprobe=1 in it, then:
//   node scripts/heap_attach.mjs
//
// Records performance.memory ticks, the app's [heapprobe] console ticks
// (state byte counts), and two CDP heap snapshots (start + end) for offline
// diffing with scripts/heap_diff.mjs. The browser is left running.
//
// Env: BROWSER_URL (http://127.0.0.1:9222), DURATION_MIN (10),
//      URL_SUBSTR (4173), OUT_DIR (agent/profile)
import fs from "node:fs";
import path from "node:path";
import puppeteer from "puppeteer-core";

const DURATION_MS = Number(process.env.DURATION_MIN || 10) * 60_000;
const BROWSER_URL = process.env.BROWSER_URL || "http://127.0.0.1:9222";
const URL_SUBSTR = process.env.URL_SUBSTR || "4173";
const HEAP_SAMPLE_MS = 5_000;
const OUT_DIR = process.env.OUT_DIR
  ? path.resolve(process.env.OUT_DIR)
  : path.resolve(new URL("..", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1"), "profile");

fs.mkdirSync(OUT_DIR, { recursive: true });
const started = Date.now();
const elapsed = () => Math.round((Date.now() - started) / 1000);
const curve = [];
const probeLines = [];

const browser = await puppeteer.connect({ browserURL: BROWSER_URL });
const pages = await browser.pages();
const page = pages.find((p) => p.url().includes(URL_SUBSTR));
if (!page) {
  console.log("[heap_attach] no matching page. open pages:");
  for (const p of pages) console.log("  ", p.url());
  process.exit(1);
}
console.log(`[heap_attach] attached to ${page.url()}; running ${Math.round(DURATION_MS / 60000)} min`);

page.on("console", (message) => {
  const text = message.text();
  if (text.startsWith("[heapprobe]")) probeLines.push({ t: elapsed(), text });
});
page.on("pageerror", (error) => console.log(`[heap_attach] pageerror: ${error.message}`));

async function sampleHeap() {
  try {
    const m = await page.evaluate(() => {
      const memory = performance.memory;
      return memory ? { used: memory.usedJSHeapSize, total: memory.totalJSHeapSize } : null;
    });
    if (m) curve.push({ t: elapsed(), usedMb: Math.round(m.used / 1048576), totalMb: Math.round(m.total / 1048576) });
  } catch {
    // Page mid-navigation; the next sample recovers.
  }
}

async function takeSnapshot(name) {
  const cdp = await page.createCDPSession();
  // Chrome 154+ streams the snapshot in addHeapSnapshotChunk events; the
  // response itself carries no data.
  const chunks = [];
  cdp.on("HeapProfiler.addHeapSnapshotChunk", (params) => chunks.push(params.chunk));
  await cdp.send("HeapProfiler.takeHeapSnapshot", { reportProgress: false });
  const out = path.join(OUT_DIR, `${name}.heapsnapshot`);
  fs.writeFileSync(out, chunks.join(""));
  console.log(`[heap_attach] t=${elapsed()}s snapshot -> ${out} (${(fs.statSync(out).size / 1048576).toFixed(1)} MiB)`);
  await cdp.detach();
}

await takeSnapshot("heap-attach-1").catch((error) => console.log(`[heap_attach] snap1 failed: ${error.message}`));

const timer = setInterval(() => void sampleHeap(), HEAP_SAMPLE_MS);
await new Promise((resolve) => setTimeout(resolve, DURATION_MS));
clearInterval(timer);
await sampleHeap();
await takeSnapshot("heap-attach-2").catch((error) => console.log(`[heap_attach] snap2 failed: ${error.message}`));

const report = {
  url: page.url(),
  durationMs: DURATION_MS,
  curve,
  probe: probeLines.map((line) => JSON.parse(line.text.slice("[heapprobe] ".length))),
};
const out = path.join(OUT_DIR, "heap-attach-curve.json");
fs.writeFileSync(out, JSON.stringify(report, null, 2));
console.log(`[heap_attach] curve -> ${out}`);
const first = curve[0];
const last = curve[curve.length - 1];
const max = curve.reduce((a, b) => (b.usedMb > a.usedMb ? b : a), curve[0]);
console.log(
  `[heap_attach] heap: start ${first?.usedMb} MiB, end ${last?.usedMb} MiB, peak ${max?.usedMb} MiB @t=${max?.t}s, samples ${curve.length}`,
);
console.log(`[heap_attach] heapprobe ticks: ${probeLines.length}`);
await browser.disconnect();
