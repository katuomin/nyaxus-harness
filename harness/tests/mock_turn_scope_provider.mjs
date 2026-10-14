// Mock provider for the turn-bound approval scope e2e test.
//
// The CALLS env var is a JSON array of script entries, answered in order
// (1-based by global completion count, across runs - the mock is stateful
// per server lifetime):
//   {"name","arguments"}  -> a tool call (arguments a JSON object)
//   {"text":"..."}        -> a plain text answer (ends the tool loop)
// After the array is exhausted it answers with plain text.
//
// Usage: MOCK_PORT=9011 CALLS='[...]' node harness/tests/mock_turn_scope_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9011);
const calls = JSON.parse(process.env.CALLS || "[]");
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    req.on("data", () => {});
    req.on("end", () => {
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.on("error", () => {});
      req.socket.on("error", () => {});
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

      const entry = calls[completions - 1];
      if (entry && entry.name) {
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
                    function: { name: entry.name, arguments: JSON.stringify(entry.arguments) },
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
          choices: [{ index: 0, delta: { content: (entry && entry.text) || "done" } }],
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
  console.log(`mock turn-scope provider listening on 127.0.0.1:${port}`);
});
