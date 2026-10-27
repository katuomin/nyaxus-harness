// Camofox REST facade mock for the sticky-session e2e test.
//
// Serves the endpoints the sticky fetch flow uses (plus the ephemeral ones),
// and records per-user stats the test asserts on:
//   POST /tabs/open                {userId,url} -> new tab (navigates)
//   POST /tabs/{tabId}/navigate    {userId,url} -> navigates an EXISTING tab
//   GET  /tabs[?userId]            -> the live tabs (with url + title)
//   GET  /snapshot?userId&targetId&offset -> the a11y text (embeds the tab's
//                                     url, so the test proves which page the
//                                     snapshot read)
//   GET  /tabs/{tabId}/screenshot  -> 1x1 PNG frame
//   DELETE /tabs/{tabId}           -> close (the reaper / ephemeral flow)
//   GET  /stats                    -> {opened, deleted, navigated, tabs, users}
//
// Usage: MOCK_PORT=9102 node harness/tests/mock_camofox_sticky_server.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9102);
let opened = 0;
let deleted = 0;
let navigated = 0;
const tabs = new Map();      // tabId -> {userId, url, title}
const users = new Set();

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    const payload = JSON.stringify({ ok: true, engine: "mock-camofox-sticky" });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/tabs/open") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      const parsed = JSON.parse(body);
      opened += 1;
      users.add(parsed.userId);
      const id = `tab-${opened}`;
      tabs.set(id, { userId: parsed.userId, url: parsed.url, title: "Sticky Test Page" });
      const payload = JSON.stringify({ tabId: id, title: "Sticky Test Page", url: parsed.url });
      res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
      res.end(payload);
    });
    return;
  }

  if (req.method === "POST" && req.url && /^\/tabs\/[^/]+\/navigate$/.test(req.url)) {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      const tabId = req.url.split("/")[2];
      const parsed = JSON.parse(body);
      const tab = tabs.get(tabId);
      if (!tab) {
        res.writeHead(404, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: `no such tab: ${tabId}` }));
        return;
      }
      navigated += 1;
      users.add(parsed.userId);
      tab.url = parsed.url;
      const payload = JSON.stringify({ ok: true, tabId, url: parsed.url, httpStatus: 200, navigationOk: true });
      res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
      res.end(payload);
    });
    return;
  }

  if (req.method === "GET" && (req.url === "/tabs" || (req.url && req.url.startsWith("/tabs?")))) {
    const payload = JSON.stringify({
      running: true,
      tabs: [...tabs.entries()].map(([id, tab]) => ({
        tabId: id, url: tab.url, title: tab.title, listItemId: "default",
      })),
    });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url && req.url.startsWith("/snapshot")) {
    const params = new URL(req.url, "http://localhost").searchParams;
    const offset = Number(params.get("offset") || 0);
    const tab = tabs.get(params.get("targetId") || "");
    // Slow snapshot read: keeps each sticky fetch in flight long enough
    // that a concurrent wave's second fetch observes the tab busy.
    setTimeout(() => {
      // The snapshot embeds the tab's CURRENT url: after a navigate the daemon
      // must read the navigated page, not the tab's first one.
      const snapshot = offset === 0 && tab
        ? `Sticky rendered the page at ${tab.url}`
        : "";
      const payload = JSON.stringify({ snapshot, url: tab ? tab.url : "", totalChars: snapshot.length });
      res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
      res.end(payload);
    }, 700);
    return;
  }

  if (req.method === "GET" && req.url && req.url.startsWith("/tabs/") && req.url.includes("/screenshot")) {
    const payload = JSON.stringify({
      screenshot: {
        data: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
        mimeType: "image/png",
      },
    });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "DELETE" && req.url && req.url.startsWith("/tabs/")) {
    const tabId = req.url.split("/")[2].split("?")[0];
    deleted += 1;
    tabs.delete(tabId);
    const payload = JSON.stringify({ ok: true });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url === "/stats") {
    const payload = JSON.stringify({
      opened, deleted, navigated,
      liveTabs: [...tabs.keys()],
      users: [...users],
    });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: `not found: ${req.method} ${req.url}`, opened, deleted }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock camofox (sticky) listening on 127.0.0.1:${port}`);
});
