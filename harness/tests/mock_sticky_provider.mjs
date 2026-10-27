// Minimal OpenAI-compatible mock provider for the sticky camofox e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: web.fetch .../page-a   (opens the sticky tab)
//     request 2: plain text (ends run 1)
//     request 3: web.fetch .../page-b   (navigates the sticky tab)
//     request 4: plain text (ends run 2)
//     request 5: TWO web.fetch calls .../page-c + .../page-d in one response
//                (the tool wave runs them concurrently: one navigates, one
//                must open its own tab)
//     request 6: plain text (ends run 3)
//     request 7: web.fetch .../page-e   (after ttl=0: the reaper closes the
//                idle tabs, then this opens fresh)
//     request 8: plain text (ends run 4)
//
// Usage: MOCK_PORT=9103 CAMOFOX_PORT=9102 node harness/tests/mock_sticky_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9103);
const camofoxPort = process.env.CAMOFOX_PORT || "9102";
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
      const toolCall = (index, id, name, args) => ({
        id,
        object: "chat.completion.chunk",
        choices: [
          {
            index: 0,
            delta: {
              tool_calls: [
                { index, id, type: "function", function: { name, arguments: JSON.stringify(args) } },
              ],
            },
          },
        ],
      });
      const text = (value) => {
        send({
          id: `mock-sticky-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: value } }],
        });
      };

      const page = `https://127.0.0.1:${camofoxPort}`;
      if (completions === 1) send(toolCall(0, "call_a", "web.fetch", { url: `${page}/page-a` }));
      else if (completions === 2) text("run one done.");
      else if (completions === 3) send(toolCall(0, "call_b", "web.fetch", { url: `${page}/page-b` }));
      else if (completions === 4) text("run two done.");
      else if (completions === 5) {
        send(toolCall(0, "call_c", "web.fetch", { url: `${page}/page-c` }));
        send(toolCall(1, "call_d", "web.fetch", { url: `${page}/page-d` }));
      }
      else if (completions === 6) text("run three done.");
      else if (completions === 7) send(toolCall(0, "call_e", "web.fetch", { url: `${page}/page-e` }));
      else text("run four done.");

      send({
        id: `mock-sticky-${completions}`,
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
  console.log(`mock sticky provider listening on 127.0.0.1:${port}`);
});
