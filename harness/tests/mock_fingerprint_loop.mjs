// Mock OpenAI-compatible provider for the fingerprint-rulebook e2e test.
//
// Behavior (stateful, per server lifetime): the first TOOLS_N
// /v1/chat/completions requests each answer with a "terminal" tool call
// carrying the CMD environment variable; every later request answers with
// plain text, ending the daemon's tool loop. Each test phase re-arms the
// mock with a known command (restart the process with a new CMD).
//
// Usage: MOCK_PORT=8971 TOOLS_N=1 CMD="git commit -m x" node harness/tests/mock_fingerprint_loop.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8971);
const toolsN = Number(process.env.TOOLS_N || 1);
const command = process.env.CMD || "git status";
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

      if (completions <= toolsN) {
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
                    function: { name: "terminal", arguments: JSON.stringify({ command }) },
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
  console.log(`mock fingerprint loop listening on 127.0.0.1:${port} (tools: ${toolsN}, cmd: ${command})`);
});
