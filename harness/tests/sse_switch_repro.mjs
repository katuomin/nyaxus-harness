// Repro for the ~5s "connection interrupted" window on session switch.
//
// Mimics the browser: one shared keep-alive connection pool to the vite dev
// server, an EventSource-like SSE stream opened, closed, then a second SSE
// stream opened on the same pool. Compares against a fresh pool.
//
// Usage: node harness/tests/sse_switch_repro.mjs <sessionId>
import http from "node:http";

const host = "127.0.0.1";
const port = 4173;
const sid = process.argv[2] || "session-1788276027393827000";

function open(agent, label) {
  return new Promise((resolve) => {
    const t0 = Date.now();
    const req = http.get(
      { host, port, path: `/api/v1/sessions/${sid}/events`, agent, headers: { accept: "text/event-stream" } },
      (res) => {
        const t = Date.now() - t0;
        res.resume();
        resolve({ label, t, status: res.statusCode });
        // hold briefly then destroy, like the browser closing the EventSource
        setTimeout(() => res.destroy(), 500);
      },
    );
    req.on("error", (e) => resolve({ label, t: Date.now() - t0, error: String(e) }));
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const shared = new http.Agent({ keepAlive: true });

// Prime the pool with a normal request, like the page's fetches.
await new Promise((r) => {
  const req = http.get({ host, port, path: `/api/v1/sessions/${sid}`, agent: shared }, (res) => {
    res.resume();
    res.on("end", r);
  });
  req.on("error", r);
});
console.log("primed pool");

// First SSE on the shared pool (like the session you were viewing).
const first = await open(shared, "first SSE (shared pool)");
console.log(first);

// Let idle pooled connections age past Node's default keepAliveTimeout (5s),
// matching real usage: you read the conversation for a while, then switch.
await sleep(6500);

// The switch: close happened, new SSE on the same (now stale) pool.
const second = await open(shared, "second SSE (shared pool, after 6.5s idle)");
console.log(second);

// Control: a brand-new pool, no stale sockets.
const fresh = new http.Agent({ keepAlive: true });
const control = await open(fresh, "second SSE (fresh pool)");
console.log(control);

process.exit(0);
