// Minimal Camofox REST facade mock for the live-browse-card e2e test.
//
// Serves the three endpoints web_fetch_via_camofox uses:
//   POST /tabs/open                -> {"tabId","title"} (the title is what
//                                      the tool.progress card must carry)
//   GET  /snapshot?userId&targetId&offset -> {"snapshot","url"} (one page,
//                                      empty on offset > 0 so the daemon's
//                                      pagination loop stops)
//   DELETE /tabs/{tabId}           -> 200 (tab lifecycle)
//
// Usage: MOCK_PORT=9046 node harness/tests/mock_camofox_server.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9046);
const pageUrl = process.env.PAGE_URL || "http://127.0.0.1:9046/page";
let opened = 0;
let deleted = 0;
let shots = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    const payload = JSON.stringify({ ok: true, engine: "mock-camofox" });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/tabs/open") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      opened += 1;
      const payload = JSON.stringify({
        tabId: `tab-${opened}`,
        title: "Camofox Test Page",
        url: pageUrl,
      });
      res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
      res.end(payload);
    });
    return;
  }

  if (req.method === "GET" && req.url && req.url.startsWith("/snapshot")) {
    // Offset pagination: the first pass returns the page, later passes
    // return an empty snapshot so the daemon's loop terminates.
    const offset = Number(new URL(req.url, "http://localhost").searchParams.get("offset") || 0);
    const snapshot = offset === 0
      ? "Camofox rendered the test page for the browse card."
      : "";
    const payload = JSON.stringify({ snapshot, url: pageUrl, totalChars: snapshot.length });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url && req.url.startsWith("/tabs/") && req.url.includes("/screenshot")) {
    shots += 1;
    // A 1x1 transparent PNG, the way the real facade returns one.
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
    deleted += 1;
    const payload = JSON.stringify({ ok: true });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: `not found: ${req.method} ${req.url}`, opened, deleted }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock camofox listening on 127.0.0.1:${port}`);
});
