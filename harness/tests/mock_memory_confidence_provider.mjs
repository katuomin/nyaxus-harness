// Minimal OpenAI-compatible mock provider for the memory confidence-lifecycle
// e2e test (design principle 5).
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model
//   POST /v1/chat/completions  -> SSE stream
//     request 1: memory.use call with id "mem:1"      (low-trust use, +0.05)
//     request 2: memory.act call with id "mem:1"      (low-trust acted-on, +0.05)
//     request 3+: a text answer citing [mem:1]        (verifiable cited, +0.10)
//
// A unit seeded at the default confidence 0.5 therefore lands at
// 0.5 + 0.05 + 0.05 + 0.10 = 0.70 after the run.
//
// Usage: MOCK_PORT=9009 node harness/tests/mock_memory_confidence_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9009);
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
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const tool = (id, name, args) => ({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [
          {
            index: 0,
            delta: {
              tool_calls: [{ index: 0, id, function: { name, arguments: JSON.stringify(args) } }],
            },
          },
        ],
      });

      if (completions === 1) {
        send(tool("call_1", "memory.use", { ids: "mem:1" }));
      } else if (completions === 2) {
        send(tool("call_2", "memory.act", { ids: "mem:1" }));
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "The zephyr listener uses port 4242 [mem:1]." } }],
        });
      }

      send({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [],
        usage: {
          prompt_tokens: 120 + completions * 10,
          completion_tokens: 12 + completions,
          total_tokens: 132 + completions * 11,
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
  console.log(`mock provider listening on 127.0.0.1:${port}`);
});
