// Mock provider for the interrupted-turn continue test (docs/stream-rules).
//
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> scripted responses:
//     1: a partial text chunk, then the stream CLOSES WITHOUT [DONE] - the
//        daemon fails the run with "provider closed the stream before
//        [DONE]" and persists the partial answer (the interrupted-turn bug
//        report's exact shape).
//     2+: a complete normal stream (the continuation run).
//
//   GET /requests -> the recorded request bodies, so the test can assert
//   that the continuation request carries the partial text as an assistant
//   message and the [CONTINUATION] note as the last message.
//
// Usage: MOCK_PORT=9077 node harness/tests/mock_continue_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9077);
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
        id: `mock-continue-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });
      if (completions === 1) {
        // The interrupted stream: text delivered, then the connection ends
        // without the [DONE] sentinel. No usage chunk either - a hard close.
        send(chunk({ content: "Batch 1 core done, the r9700 data is excellent. Continuing with the KV cluster and the" }));
        res.end();
        return;
      }
      send(chunk({ content: "KV cluster and the model card are done, results attached." }));
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock continue provider listening on 127.0.0.1:${port}`);
});
