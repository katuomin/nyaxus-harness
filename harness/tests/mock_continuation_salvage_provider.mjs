// Mock provider for the continuation-salvage test (docs/stream-rules).
//
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses:
//     1: run 1 - stream a fragment, then HOLD the SSE stream open (no [DONE])
//        for a long while - the test kills the daemon mid-call so the partial
//        exists only as ledger delta events.
//     2: run 2 (the continuation of run 1) - stream a fragment, then HOLD the
//        same way - the test kills the daemon mid-continuation.
//     3+: run 3 (a fresh prompt queued while run 2 streamed) - a complete
//        normal stream; it revives after the restart and completes.
//
//   GET /requests -> the recorded request bodies.
//
// Usage: MOCK_PORT=9097 node harness/tests/mock_continuation_salvage_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9097);
let completions = 0;
const requests = [];

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url === "/requests") {
    const payload = JSON.stringify(requests);
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
      try {
        requests.push(JSON.parse(body));
      } catch {
        requests.push({ unparseable: true });
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const chunk = (delta) => ({
        id: `mock-continuation-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });
      if (completions === 1) {
        send(chunk({ content: "First fragment survived the restart" }));
        res.write(`data: ${JSON.stringify({ id: "hold", object: "chat.completion.chunk", choices: [{ index: 0, delta: {} }] })}\n\n`);
        setTimeout(() => {
          try {
            res.end();
          } catch {
            // the client died mid-hold; nothing to do
          }
        }, 60000);
        return;
      }
      if (completions === 2) {
        send(chunk({ content: "Continuation fragment survived the restart" }));
        res.write(`data: ${JSON.stringify({ id: "hold", object: "chat.completion.chunk", choices: [{ index: 0, delta: {} }] })}\n\n`);
        setTimeout(() => {
          try {
            res.end();
          } catch {
            // the client died mid-hold; nothing to do
          }
        }, 60000);
        return;
      }
      send(chunk({ content: "Third: the queued run revived and completed." }));
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock continuation salvage provider listening on 127.0.0.1:${port}`);
});
