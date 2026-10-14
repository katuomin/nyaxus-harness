// Dump the call chain for the hottest CDP CPU-profile nodes (self-time), so a
// confusing self-attribution can be traced to its real caller.
//
//   node scripts/cpu_chain.mjs [seconds]   (default 30)
import fs from "node:fs";
import puppeteer from "puppeteer-core";

const SECONDS = Number(process.argv[2] || 30);
const browser = await puppeteer.connect({ browserURL: process.env.BROWSER_URL || "http://127.0.0.1:9222" });
const pages = await browser.pages();
const page = pages.find((p) => p.url().includes("4173"));
if (!page) { console.log("no nyaxus page found"); process.exit(1); }
const cdp = await page.createCDPSession();
await cdp.send("Profiler.enable");
await cdp.send("Profiler.start");
await new Promise((r) => setTimeout(r, SECONDS * 1000));
const { profile } = await cdp.send("Profiler.stop");
await cdp.send("Profiler.disable");

const byId = new Map(profile.nodes.map((n) => [n.id, n]));
const selfUs = new Map();
profile.samples.forEach((nodeId, i) => {
  selfUs.set(nodeId, (selfUs.get(nodeId) || 0) + profile.timeDeltas[i] * 1000);
});
const chainOf = (node) => {
  const chain = [];
  let cur = node;
  const seen = new Set();
  while (cur && !seen.has(cur.id)) {
    seen.add(cur.id);
    chain.push(cur.callFrame.functionName || "(anon)");
    cur = cur.parent !== undefined ? byId.get(cur.parent) : undefined;
  }
  return chain.join(" <- ");
};
const top = [...selfUs.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8);
for (const [nodeId, us] of top) {
  const node = byId.get(nodeId);
  if (!node) continue;
  const frame = node.callFrame;
  console.log(`${(us / 1000).toFixed(0)}ms  ${frame.functionName || "(anon)"} ${(frame.url || "").split("/").slice(-1)[0]}:${frame.lineNumber}`);
  console.log(`   chain: ${chainOf(node)}`);
}
await browser.disconnect();
