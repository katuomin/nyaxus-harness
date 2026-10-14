// Mock OpenAI-compatible provider for the task-checklist e2e test.
//
// Behavior (stateful, per server lifetime): the first N /v1/chat/completions
// requests each answer with the next tool call from the TOOLS environment
// variable (a JSON array of {name, arguments} objects); every later request
// answers with plain text, ending the daemon's tool loop.
//
// Usage: MOCK_PORT=8971 TOOLS='[{"name":"tasks.create","arguments":{"tasks":"[...]"}},...]' node harness/tests/mock_tasks_loop.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8971);
let tools = [];
try {
  tools = JSON.parse(process.env.TOOLS || "[]");
} catch {
  tools = [];
}
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

      const tool = tools[completions - 1];
      if (tool) {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: `call_${completions}`,
                    function: { name: tool.name, arguments: JSON.stringify(tool.arguments) },
                  },
                ],
              },
            },
          ],
        });
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "done" } }],
        });
      }

      send({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [],
        usage: {
          prompt_tokens: 100 + completions * 10,
          completion_tokens: 12 + completions,
          total_tokens: 112 + completions * 11,
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
  console.log(`mock tasks loop listening on 127.0.0.1:${port} (tools: ${tools.length})`);
});
