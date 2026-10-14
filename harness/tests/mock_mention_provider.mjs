// Minimal OpenAI-compatible mock provider for the @-mention / terminal
// truncation e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> appends the request body as one JSON line to
//                                 MENTION_REQUESTS_FILE (the test greps the
//                                 injected referenced-paths note out of it),
//                                 then responds by completion count:
//     1: plain text "ok mentions" (the @-mention turn, no tools)
//     2: terminal tool call `awk '{print}' big.txt` (the truncation turn; the test
//        writes a >256 KiB file into the workspace first)
//     3: plain text "done"
//
// Usage: MENTION_REQUESTS_FILE=... MOCK_PORT=9037 node harness/tests/mock_mention_provider.mjs
import http from "node:http";
import fs from "node:fs";

const port = Number(process.env.MOCK_PORT || 9037);
const requestsFile = process.env.MENTION_REQUESTS_FILE || "";
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
      if (requestsFile) fs.appendFileSync(requestsFile, body + "\n");
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const toolCall = (id, name, args) => ({
        id: `mock-${completions}`,
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

      if (completions === 2) {
        // awk (not cat/type): classifier-gated, so the turn parks at the
        // approval gate as the test expects; it prints the whole file.
        send(toolCall("call_big", "terminal", { command: "awk '{print}' big.txt" }));
      } else {
        const text = completions === 1 ? "ok mentions" : "done";
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: { content: text },
            },
          ],
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
  console.log(`mock mention provider listening on 127.0.0.1:${port}`);
});
