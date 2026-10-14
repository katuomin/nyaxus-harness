// Mock provider for the deny-guard + approval-TTL e2e tests.
//
// The CALLS env var is a JSON array of {"name","arguments"} (arguments a
// JSON object). Request i (1-based) answers with CALLS[i-1] as a tool call
// while i is in range; after the array is exhausted it answers with plain
// text, ending the daemon's tool loop.
//
// Usage: MOCK_PORT=9001 CALLS='[{"name":"terminal","arguments":{"command":"rm -rf /"}}]' \
//   node harness/tests/mock_guard_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9001);
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

      const call = calls[completions - 1];
      if (call) {
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
                    function: { name: call.name, arguments: JSON.stringify(call.arguments) },
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
  console.log(`mock guard provider listening on 127.0.0.1:${port}`);
});
