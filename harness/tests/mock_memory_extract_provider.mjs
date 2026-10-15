// Mock memory-extraction endpoint for the memory_extract e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so discovery succeeds
//   POST /v1/chat/completions  -> SSE stream with a fixed candidate list:
//     * world      (grounded in the turn)      -> must be stored
//     * procedure  (grounded in the turn)      -> must be stored
//     * bogus      (unrecognized type)         -> must be rejected
//     * lesson     (no token overlap)          -> must be rejected (ungrounded)
//
// The candidate texts are chosen against the test's seed turn so the
// grounding check (>=70% of the candidate's >=4-char tokens present in the
// source) passes for the first two and fails for the last.
//
// Usage: MOCK_PORT=9032 node harness/tests/mock_memory_extract_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9032);

const CANDIDATES = {
  candidates: [
    { type: "world", text: "The zephyr protocol listener binds port 4242 on the gateway.", confidence: 0.9 },
    { type: "procedure", text: "Run make check to verify the build.", confidence: 0.8 },
    { type: "bogus", text: "The relay beacon uses a copper coil.", confidence: 0.9 },
    { type: "lesson", text: "The qurix lattice emits a violet pulse.", confidence: 0.7 },
  ],
};

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ data: [{ id: "mock-extract", meta: { n_ctx: 4096 } }] }));
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
      res.write(
        `data: ${JSON.stringify({
          id: "mock-extract",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: JSON.stringify(CANDIDATES) } }],
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
  console.log(`mock extract provider listening on 127.0.0.1:${port}`);
});
