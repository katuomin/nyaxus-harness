// Mock provider for the rapid-fire e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> after MOCK_DELAY_MS, stream a single text
//     turn reporting how many user/assistant messages it saw and the last
//     user content. The delay keeps the first run in flight long enough for
//     the test to send a second message into the queue.
//
// Usage: MOCK_PORT=8921 MOCK_DELAY_MS=2500 node harness/tests/mock_slow_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8921);
const delayMs = Number(process.env.MOCK_DELAY_MS || 2500);

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's get_body client reads until EOF
    // and does not decode chunked transfer encoding.
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
      setTimeout(() => {
        let text = "no user message";
        try {
          const parsed = JSON.parse(body);
          const messages = parsed.messages || [];
          const users = messages.filter((m) => m.role === "user");
          const assistants = messages.filter((m) => m.role === "assistant");
          const last = users.length ? users[users.length - 1].content : "";
          text = `users: ${users.length}, assistants: ${assistants.length}, last: ${last}`;
        } catch {
          // Malformed request; keep the default answer.
        }
        res.writeHead(200, {
          "content-type": "text/event-stream",
          "cache-control": "no-cache",
        });
        res.write(
          `data: ${JSON.stringify({
            id: "mock-slow",
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta: { content: text } }],
          })}\n\n`,
        );
        res.write("data: [DONE]\n\n");
        res.end();
      }, delayMs);
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock slow provider listening on 127.0.0.1:${port} (delay=${delayMs}ms)`);
});
