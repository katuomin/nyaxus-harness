// Sample Chromium performance counters on the nyaxus page via CDP to find
// what grows while the tab is "working" (layout/style/paint churn, DOM node
// count, JS heap). Attach to a running browser on the debug port.
//
//   node scripts/perf_metrics.mjs [seconds]   (default 90)
import puppeteer from "puppeteer-core";

const SECONDS = Number(process.argv[2] || 90);
const browser = await puppeteer.connect({ browserURL: process.env.BROWSER_URL || "http://127.0.0.1:9222" });
const pages = await browser.pages();
const page = pages.find((p) => p.url().includes("4173"));
if (!page) {
  console.log("no nyaxus page found");
  process.exit(1);
}
const cdp = await page.createCDPSession();
await cdp.send("Performance.enable");

const getMetrics = async () => (await cdp.send("Performance.getMetrics")).metrics;
const pick = (m) =>
  Object.fromEntries(
    m
      .filter((x) =>
        [
          "JSHeapUsedSize",
          "JSHeapSizeLimit",
          "Nodes",
          "LayoutCount",
          "LayoutDuration",
          "RecalcStyleCount",
          "RecalcStyleDuration",
          "PaintCount",
          "PaintDuration",
          "Documents",
        ].includes(x.name),
      )
      .map((x) => [x.name, Math.round(x.value * 1000) / 1000]),
  );

const t0 = pick(await getMetrics());
console.log(`[perf_metrics] t0:`, JSON.stringify(t0));
await new Promise((r) => setTimeout(r, SECONDS * 1000));
const t1 = pick(await getMetrics());
console.log(`[perf_metrics] t${SECONDS}s:`, JSON.stringify(t1));

const deltas = {};
for (const key of Object.keys(t1)) {
  deltas[key] = t1[key] - t0[key];
}
console.log(`[perf_metrics] delta over ${SECONDS}s:`, JSON.stringify(deltas));
await cdp.detach();
await browser.disconnect();
