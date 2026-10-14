// Mock provider for the named-provider routing e2e test.
//
// Two instances of this server (A = the daemon's default provider, B = the
// named provider) run side by side. Each records every chat completion
// request it receives and exposes it for assertions:
//
//   GET  /v1/models            -> one model (MODEL_NAME env), so discovery works
//   POST /v1/chat/completions  -> records the parsed request body + auth
//                                 header, answers one text chunk, [DONE]
//   GET  /stats                -> { model, completions, last_request, last_auth }
//
// The test uses /stats to prove a profile's named provider routes to the
// right endpoint with the right model, sampling params, custom params, and
// API key - and that the default provider was never contacted.
//
// Usage: MOCK_PORT=8911 MODEL_NAME=mock-model-a node harness/tests/mock_routing_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8911);
const model = process.env.MODEL_NAME || "mock-model";
let completions = 0;
let lastRequest = null;
let lastAuth = null;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: model, meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url === "/stats") {
    const payload = JSON.stringify({
      model,
      completions,
      last_request: lastRequest,
      last_auth: lastAuth,
    });
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
      lastAuth = req.headers["authorization"] || null;
      try {
        lastRequest = JSON.parse(body);
      } catch {
        lastRequest = { raw: body };
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.write(
        `data: ${JSON.stringify({
          id: "mock-routing",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "routed-ok" } }],
        })}\n\n`,
      );
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock routing provider (${model}) listening on 127.0.0.1:${port}`);
});
