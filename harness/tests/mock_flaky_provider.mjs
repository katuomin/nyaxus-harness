// Mock provider for the stream-failure e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> the first FAIL_TIMES requests answer 503
//     (llama.cpp "server busy"), then stream a single text turn echoing the
//     last user message, like the attachment mock.
//
// With FAIL_TIMES=1 the daemon's single retry should recover and the run
// completes; with FAIL_TIMES=999 every attempt fails and the run must end
// run.failed with a 503 error, not a phantom run.completed.
//
// Usage: MOCK_PORT=8913 FAIL_TIMES=1 node harness/tests/mock_flaky_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8913);
const failTimes = Number(process.env.FAIL_TIMES || 1);
let completions = 0;

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
      completions += 1;
      if (completions <= failTimes) {
        // llama.cpp's busy response.
        const payload = JSON.stringify({ error: { message: "server is busy processing requests", type: "server_error" } });
        res.writeHead(503, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
        res.end(payload);
        return;
      }
      let text = "no user message";
      try {
        const parsed = JSON.parse(body);
        const users = (parsed.messages || []).filter((m) => m.role === "user");
        if (users.length) text = `user: ${users[users.length - 1].content}`;
      } catch {
        // Malformed request; keep the default answer.
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.write(
        `data: ${JSON.stringify({
          id: "mock-flaky",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: text } }],
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
  console.log(`mock flaky provider listening on 127.0.0.1:${port} (FAIL_TIMES=${failTimes})`);
});
