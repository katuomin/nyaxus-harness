// Mock provider for the orphan-salvage test (docs/stream-rules).
//
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses:
//     1: two text delta chunks, then HOLD the SSE stream open (no [DONE],
//        no close) for a long while - the test kills the daemon mid-call.
//        The run's streamed text exists only as ledger delta events.
//     2+: a complete normal stream (the continue's continuation run).
//
//   GET /requests -> the recorded request bodies.
//
// Usage: MOCK_PORT=9079 node harness/tests/mock_salvage_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9079);
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
        id: `mock-salvage-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });
      if (completions === 1) {
        // Stream a fragment, then hold the connection without [DONE].
        send(chunk({ content: "Salvage fragment survived the restart" }));
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
      send(chunk({ content: "Continued: batch 7 resumed from the anchor." }));
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock salvage provider listening on 127.0.0.1:${port}`);
});
