// Minimal OpenAI-compatible mock provider for the memory cited/acted-on e2e
// test (design principle 5).
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: memory.act call with ids "mem:1, mem:2"
//                (mem:1 is offered this turn; mem:2 is out of scope for the
//                 session and must be rejected by the daemon's offered-set
//                 validation)
//     request 2+: a text answer that cites [mem:1] (offered -> recorded) and
//                 [mem:2] (not offered -> rejected by offered-set validation)
//
// Tool-call arguments follow the OpenAI convention: a JSON object string
// ("{\"ids\":\"...\"}"), as the daemon's dispatch requires.
//
// Usage: MOCK_PORT=9007 node harness/tests/mock_memory_cite_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9007);
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
                      name: "memory.act",
                      arguments: JSON.stringify({ ids: "mem:1, mem:2" }),
                    },
                  },
                ],
              },
            },
          ],
        });
      } else {
        // Final answer cites mem:1 (offered this turn) and mem:2 (not offered).
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                content:
                  "The zephyr listener uses port 4242 [mem:1]; the quasar " +
                  "relay uses 5555 [mem:2].",
              },
            },
          ],
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
