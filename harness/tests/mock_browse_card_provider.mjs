// Minimal OpenAI-compatible mock provider for the live-browse-card e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: web.fetch tool call (an https url whose native TLS
//                handshake fails against the plain-HTTP mock, so the
//                daemon escalates to the camofox tier - the escalation
//                path the render tier exists for)
//     request 2: a plain text answer (ends the tool loop)
//
// Usage: MOCK_PORT=9047 CAMOFOX_PORT=9046 node harness/tests/mock_browse_card_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9047);
const camofoxPort = process.env.CAMOFOX_PORT || "9046";
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
      const toolCall = (id, name, args) => ({
        id,
        object: "chat.completion.chunk",
        choices: [
          {
            index: 0,
            delta: {
              tool_calls: [
                { index: 0, id, function: { name, arguments: JSON.stringify(args) } },
              ],
            },
          },
        ],
      });

      if (completions === 1) {
        send(toolCall("call_fetch_camo", "web.fetch", {
          url: `https://127.0.0.1:${camofoxPort}/page`,
        }));
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                content: "the camofox page said it rendered the test page.",
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
