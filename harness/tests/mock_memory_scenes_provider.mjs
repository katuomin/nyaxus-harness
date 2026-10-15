// Mock memory endpoint for the memory_scenes e2e test. Serves the
// extraction, scene-refresh, and persona-refresh calls on one port; the
// daemon sends all three to memory.extract_endpoint, so the mock
// distinguishes them by the system prompt.
//
// Behavior:
//   GET  /v1/models            -> one model, so discovery succeeds
//   POST /v1/chat/completions:
//     * system has "consolidation architect" -> scenes mode: two fixed
//       creates (one per seeded topic)
//     * system has "persona architect" -> persona mode: fixed persona JSON
//     * otherwise -> extract mode: empty candidates (the test seeds units
//       through record approval, which never calls the extractor)
//
// Usage: MOCK_PORT=9066 node harness/tests/mock_memory_scenes_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9066);

const SCENES_OUTPUT = JSON.stringify([
  {
    action: "create",
    name: "coffee-ritual",
    summary: "The user's morning coffee practice",
    heat: 1,
    content:
      "# Coffee\n\nThe user drinks Ethiopian coffee every morning, " +
      "ground with a hand mill, brewed with a gooseneck kettle.",
  },
  {
    action: "create",
    name: "c-plus-plus-work",
    summary: "The user's C++ development context",
    heat: 1,
    content:
      "# C++ Work\n\nThe user develops the nyaxus harness in C++ " +
      "with minimal dependencies.",
  },
]);

const PERSONA_OUTPUT = JSON.stringify({
  persona:
    "# User\n\nThe user drinks Ethiopian coffee and develops the " +
    "nyaxus harness in C++.",
});

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ data: [{ id: "mock-scenes", meta: { n_ctx: 4096 } }] }));
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      let payload = {};
      try {
        payload = JSON.parse(body);
      } catch {
        payload = {};
      }
      const messages = payload.messages || [];
      const system = (messages[0] && messages[0].content) || "";
      let content;
      if (system.includes("consolidation architect")) {
        content = SCENES_OUTPUT;
      } else if (system.includes("persona architect")) {
        content = PERSONA_OUTPUT;
      } else {
        content = JSON.stringify({ candidates: [] });
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.write(
        `data: ${JSON.stringify({
          id: "mock-scenes",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content } }],
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
  console.log(`mock scenes provider listening on 127.0.0.1:${port}`);
});
