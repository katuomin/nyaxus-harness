// Mock provider for the title retitle e2e test: a single text turn with a
// substantive reply, so the opening exchange (user + assistant) gives the
// title model something to anchor on.
//
// Usage: MOCK_PORT=8970 node harness/tests/mock_title_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8970);

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    req.resume();
    req.on("end", () => {
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const text = "I can run terminal commands in the workspace, search the long-term memory bank for durable facts, and store new ones for later sessions.";
      res.write(
        `data: ${JSON.stringify({
          id: "mock-title",
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
  console.log(`mock title provider listening on 127.0.0.1:${port}`);
});
