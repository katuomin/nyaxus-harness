// Headless heap profiler for the nyaxus web UI.
//
// Drives the real UI (vite dev + local daemon) in a throwaway Chromium
// profile, runs a scripted heavy session (repeated prompts that induce tool
// calls + reasoning, auto-approving every gate), and records:
//   - performance.memory used/total every HEAP_SAMPLE_MS
//   - the app's [heapprobe] console ticks (state byte counts)
//   - two V8 heap snapshots (at SNAPSHOT_1_MS and at the end) for offline
//     diffing with scripts/heap_diff.mjs
//
// Usage:
//   node scripts/heap_profile.mjs
// Env:
//   DURATION_MIN   run length (default 12)
//   PROMPT_EVERY_MS new prompt cadence (default 75000)
//   URL            default http://localhost:5173/?heapprobe=1
//   OUT_DIR        default agent/profile
import { createRequire } from "node:module";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import puppeteer from "puppeteer-core";

const require = createRequire(import.meta.url);
void require; // puppeteer-core is ESM-importable via the package's exports

const DURATION_MS = Number(process.env.DURATION_MIN || 12) * 60_000;
const PROMPT_EVERY_MS = Number(process.env.PROMPT_EVERY_MS || 75_000);
const HEAP_SAMPLE_MS = 5_000;
const SNAPSHOT_1_MS = 90_000;
const URL = process.env.URL || "http://localhost:5173/?heapprobe=1";
const OUT_DIR = process.env.OUT_DIR
  ? path.resolve(process.env.OUT_DIR)
  : path.resolve(new URL("..", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1"), "profile");
const BROWSER =
  process.env.BROWSER_EXE ||
  "C:/Users/<user>/AppData/Local/imput/Helium/Application/chrome.exe";

const PROMPT =
  "For i from 1 to 5: run the terminal command `echo probe-i` one at a time. " +
  "After each run, write three or four sentences explaining what the output " +
  "means and why it matters. When all five are done, summarize the outputs.";

fs.mkdirSync(OUT_DIR, { recursive: true });
const profileDir = fs.mkdtempSync(path.join(os.tmpdir(), "nyaxus-heap-"));
const started = Date.now();
const elapsed = () => Math.round((Date.now() - started) / 1000);

const curve = [];
const probeLines = [];

async function sampleHeap(page) {
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

async function takeSnapshot(page, name) {
  const cdp = await page.createCDPSession();
  // Chrome 154+ streams the snapshot in addHeapSnapshotChunk events; the
  // response itself carries no data.
  const chunks = [];
  cdp.on("HeapProfiler.addHeapSnapshotChunk", (params) => chunks.push(params.chunk));
  await cdp.send("HeapProfiler.takeHeapSnapshot", { reportProgress: false });
  const out = path.join(OUT_DIR, `${name}.heapsnapshot`);
  fs.writeFileSync(out, chunks.join(""));
  console.log(`[heap_profile] snapshot -> ${out} (${(fs.statSync(out).size / 1048576).toFixed(0)} MiB)`);
  await cdp.detach();
}

async function sendPrompt(page) {
  const box = await page.$('textarea[aria-label="Message the orchestrator"]');
  if (!box) return;
  await box.click({ clickCount: 3 });
  await page.keyboard.type(PROMPT, { delay: 2 });
  await page.keyboard.press("Enter");
  console.log(`[heap_profile] t=${elapsed()}s prompt sent`);
}

async function approveGates(page) {
  const buttons = await page.$$(".approval-allow");
  for (const button of buttons) {
    try {
      await button.click();
    } catch {
      // Card unmounted between query and click.
    }
  }
}

const browser = await puppeteer.launch({
  executablePath: BROWSER,
  headless: true,
  userDataDir: profileDir,
  args: ["--no-first-run", "--no-default-browser-check", "--disable-background-timer-throttling"],
});

try {
  const page = await browser.newPage();
  page.on("console", (message) => {
    const text = message.text();
    if (text.startsWith("[heapprobe]")) probeLines.push({ t: elapsed(), text });
  });
  page.on("pageerror", (error) => console.log(`[heap_profile] pageerror: ${error.message}`));

  console.log(`[heap_profile] opening ${URL}`);
  await page.goto(URL, { waitUntil: "networkidle2", timeout: 60_000 });
  // The app lands on the overview page; open a session so the composer exists.
  await page.waitForSelector('.session-item, textarea[aria-label="Message the orchestrator"]', { timeout: 30_000 });
  if (!(await page.$('textarea[aria-label="Message the orchestrator"]'))) {
    // Some .session-item nodes live in a collapsed drawer; click a visible one via JS.
    const opened = await page.evaluate(() => {
      const items = [...document.querySelectorAll(".session-item")];
      const visible = items.find((el) => !!(el.offsetWidth || el.offsetHeight));
      if (visible) {
        visible.click();
        return true;
      }
      return false;
    });
    if (!opened) {
      // Fresh workspace: no sessions yet, so start a draft conversation.
      await page.click(".new-session-button");
      console.log(`[heap_profile] t=${elapsed()}s started new conversation`);
    } else {
      console.log(`[heap_profile] t=${elapsed()}s opened first session`);
    }
  }
  await page.waitForSelector('textarea[aria-label="Message the orchestrator"]', { timeout: 30_000 });
  console.log(`[heap_profile] UI ready at t=${elapsed()}s; running ${Math.round(DURATION_MS / 60000)} min`);

  const timers = [];
  timers.push(setInterval(() => void sampleHeap(page), HEAP_SAMPLE_MS));
  timers.push(setInterval(() => void approveGates(page), 2_000));
  timers.push(setInterval(() => void sendPrompt(page), PROMPT_EVERY_MS));
  let snap1 = false;
  timers.push(
    setInterval(() => {
      if (!snap1 && Date.now() - started >= SNAPSHOT_1_MS) {
        snap1 = true;
        void takeSnapshot(page, "heap-1").catch((error) => console.log(`[heap_profile] snap1 failed: ${error.message}`));
      }
    }, 5_000),
  );

  void sendPrompt(page);
  await new Promise((resolve) => setTimeout(resolve, DURATION_MS));

  for (const timer of timers) clearInterval(timer);
  await sampleHeap(page);
  await takeSnapshot(page, "heap-2").catch((error) => console.log(`[heap_profile] snap2 failed: ${error.message}`));

  const report = {
    url: URL,
    durationMs: DURATION_MS,
    curve,
    probe: probeLines.map((line) => JSON.parse(line.text.slice("[heapprobe] ".length))),
  };
  const out = path.join(OUT_DIR, "heap-curve.json");
  fs.writeFileSync(out, JSON.stringify(report, null, 2));
  console.log(`[heap_profile] curve -> ${out}`);
  const first = curve[0];
  const last = curve[curve.length - 1];
  const max = curve.reduce((a, b) => (b.usedMb > a.usedMb ? b : a), curve[0]);
  console.log(
    `[heap_profile] heap: start ${first?.usedMb} MiB, end ${last?.usedMb} MiB, peak ${max?.usedMb} MiB @t=${max?.t}s, samples ${curve.length}`,
  );
  console.log(`[heap_profile] heapprobe ticks: ${probeLines.length}`);
} finally {
  await browser.close();
}
