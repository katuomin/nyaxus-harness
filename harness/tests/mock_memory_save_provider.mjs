// Minimal OpenAI-compatible mock provider for the memory.save e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: memory.save with an explicit type (world: a stable
//                environment fact)
//     request 2: memory.save without a type (defaults to experience)
//     request 3+: a plain text answer (ends the daemon's tool loop)
//
// The test asserts the run completes WITHOUT any approve/deny command and
// that both facts land in the memory bank with the right types - i.e.
// memory.save no longer sits behind the per-call approval gate (the
// memory-enabled check is the only gate) and the optional type param is
// honored.
//
// Tool-call arguments follow the OpenAI convention: a JSON object string
// ("{\"text\":\"...\"}"), as the daemon's dispatch requires.
//
// Usage: MOCK_PORT=9011 node harness/tests/mock_memory_save_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9011);
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
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

      if (completions === 1) {
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
                    id: "call_1",
                    function: {
                      name: "memory.save",
                      arguments: JSON.stringify({ text: "The vault passphrase is iron-88.", type: "world" }),
                    },
                  },
                ],
              },
            },
          ],
        });
      } else if (completions === 2) {
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
                    id: "call_2",
                    function: {
                      name: "memory.save",
                      arguments: JSON.stringify({ text: "We agreed to keep the relay secret from the guild." }),
                    },
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
          choices: [{ index: 0, delta: { content: "saved" } }],
        });
      }

      // Final chunk carrying usage, like llama.cpp's OpenAI server.
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
