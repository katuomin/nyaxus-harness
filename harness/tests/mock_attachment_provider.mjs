// Mock provider for the attachments e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> a single text turn that echoes the content
//     of the last user message. Array content is checked for an OpenAI image_url
//     part so the test can verify multimodal serialization.
//
// Usage: MOCK_PORT=8911 node harness/tests/mock_attachment_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8911);

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] }));
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      let text = "no user message";
      try {
        const parsed = JSON.parse(body);
        const users = (parsed.messages || []).filter((m) => m.role === "user");
        if (users.length) {
          const content = users[users.length - 1].content;
          if (Array.isArray(content)) {
            const imageParts = content.filter((part) => part.type === "image_url");
            const hasDataUrl = imageParts.some((part) =>
              typeof part.image_url?.url === "string" &&
              part.image_url.url.startsWith("data:image/"));
            text = hasDataUrl ? "image_url part received" : "image_url part malformed";
          } else {
            text = `user: ${content}`;
          }
        }
      } catch {
        // Malformed request; keep the default answer.
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.write(
        `data: ${JSON.stringify({
          id: "mock-attachment",
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
  console.log(`mock attachment provider listening on 127.0.0.1:${port}`);
});
