// Simulates the browser's session-switch sequence against the vite dev
// server (which proxies to the daemon):
//   1. open SSE stream for session A (like the live EventSource)
//   2. close it (like source.close() in the effect cleanup)
//   3. GET the snapshot (like loadSession)
//   4. open SSE stream again (like the new EventSource)
// and measures how long step 4 takes to receive response headers.
import http from "node:http";

const port = Number(process.argv[2] || 4199);
const sid = process.argv[3] || "session-1788276027393827000";
const host = "127.0.0.1";

// Mimic the browser's per-host keep-alive pool.
const agent = new http.Agent({ keepAlive: true, keepAliveMsecs: 1000 });

const openSse = (after = 0) =>
  new Promise((resolve, reject) => {
    const t0 = Date.now();
    const req = http.get(
      {
        host,
        port,
        path: `/api/v1/sessions/${sid}/events${after ? `?after=${after}` : ""}`,
        agent,
        headers: { accept: "text/event-stream" },
      },
      (res) => {
        resolve({
          ms: Date.now() - t0,
          status: res.statusCode,
          res,
          close: () => res.destroy(),
        });
        res.on("data", () => {});
      },
    );
    req.on("error", (e) => reject(new Error(`sse request error: ${e.message}`)));
  });

const getSnapshot = () =>
  new Promise((resolve, reject) => {
    const t0 = Date.now();
    const req = http.get(
      { host, port, path: `/api/v1/sessions/${sid}`, agent },
      (res) => {
        let n = 0;
        res.on("data", (c) => (n += c.length));
        res.on("end", () => resolve({ ms: Date.now() - t0, status: res.statusCode, bytes: n }));
      },
    );
    req.on("error", (e) => reject(new Error(`snapshot error: ${e.message}`)));
  });

// 1. live SSE stream for the "current" session
const sse1 = await openSse();
console.log("sse A opened:", sse1.ms, "ms, status", sse1.status);
await new Promise((r) => setTimeout(r, 1500)); // let it live a bit

// 2. close it, like the effect cleanup does
sse1.close();
console.log("sse A closed (client side)");

// 3. snapshot GET for the target session
const snap = await getSnapshot();
console.log("snapshot:", snap.ms, "ms, status", snap.status, snap.bytes, "bytes");

// 4. new SSE stream, like the new EventSource
const sse2 = await openSse();
console.log("sse B opened:", sse2.ms, "ms, status", sse2.status);

sse2.close();
agent.destroy();
process.exit(0);
