// Mock provider for the profiles e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> a single text turn that echoes every system
//     message it received ("system: <first>" for one, joined with "==="
//     for several), or reports that none was sent. This lets the tests verify
//     that a profile's identity_prompt and injected blocks (e.g. recalled
//     memory) reach the provider, in order.
//
// Usage: MOCK_PORT=8907 node harness/tests/mock_profile_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8907);

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
      let text = "no system message";
      try {
        const parsed = JSON.parse(body);
        const systems = (parsed.messages || []).filter((m) => m.role === "system");
        if (systems.length) {
          text = "system: " + systems.map((m) => m.content).join("\n===\n");
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
          id: "mock-profile",
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
  console.log(`mock profile provider listening on 127.0.0.1:${port}`);
});
