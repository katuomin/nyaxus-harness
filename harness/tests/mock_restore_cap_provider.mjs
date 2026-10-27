// Mock provider for the approval restore-cap e2e test (the session-scope
// grant must NOT survive a daemon restart).
//
// Behavior (stateful, per server lifetime; the mock outlives the daemon):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: terminal call "rm one.txt"  (gated write; the operator
//                approves with session scope)
//     request 2: plain text (phase one ends)
//     (the test restarts the daemon here)
//     request 3: terminal call "rm one.txt"  (MUST re-gate: the session
//                grant died with the old process)
//     request 4+: plain text (phase two ends)
//
// Usage: MOCK_PORT=9088 node harness/tests/mock_restore_cap_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9088);
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

      if (completions === 1 || completions === 3) {
        send({
          id: `mock-restore-${completions}`,
          object: "chat.completion.chunk",
          choices: [{
            index: 0,
            delta: {
              tool_calls: [
                { index: 0, id: `call_rm_${completions}`, function: { name: "terminal", arguments: "{\"command\":\"rm one.txt\"}" } },
              ],
            },
          }],
        });
      } else {
        send({ id: `mock-restore-${completions}`, object: "chat.completion.chunk", choices: [{ index: 0, delta: { content: "phase done." } }] });
      }
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {});
