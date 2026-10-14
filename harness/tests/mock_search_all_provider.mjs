// Minimal OpenAI-compatible mock provider for the cross-session search e2e
// test.
//
// Behavior (stateless):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream with a plain text answer "ok"
//                                 (no tool calls: each run ends in one step)
//
// The test drives two sessions with user messages carrying a shared phrase;
// the search route must find the phrase in both sessions.
//
// Usage: MOCK_PORT=8931 node harness/tests/mock_search_all_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8931);

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's client reads until EOF and does
    // not decode chunked transfer encoding on this path.
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
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      send({
        id: "mock-1",
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta: { content: "ok" } }],
      });
      // Final chunk carrying usage, like llama.cpp's OpenAI server.
      send({
        id: "mock-1",
        object: "chat.completion.chunk",
        choices: [],
        usage: {
          prompt_tokens: 120,
          completion_tokens: 4,
          total_tokens: 124,
        },
      });
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock search-all provider listening on 127.0.0.1:${port}`);
});
