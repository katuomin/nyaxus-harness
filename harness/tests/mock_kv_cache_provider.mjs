// Mock provider for the kv_cache_miss tripwire e2e test.
//
// Every response carries a long prompt (prompt_tokens=2048, above the
// daemon's 1024 tripwire floor) and varies the cached-token report:
//
//   MODE=cold    -> text reply, usage cached_tokens=0 (known miss)
//   MODE=hit     -> text reply, usage cached_tokens=2000 (known hit)
//   MODE=regress -> request 1: terminal tool call {"command":"ls"} (read-only,
//                   auto-approved) + usage cached_tokens=2000; request 2+:
//                   text reply + usage cached_tokens=0. A hit followed by a
//                   miss in the same run - the regression kind.
//   MODE=unknown -> text reply, usage with no cache fields at all. The
//                   daemon must not fire the tripwire on "unknown".
//
// Usage: MOCK_PORT=8991 MODE=cold node harness/tests/mock_kv_cache_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8991);
const mode = process.env.MODE || "cold";
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

      if (mode === "regress" && completions === 1) {
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
                    function: { name: "terminal", arguments: JSON.stringify({ command: "ls" }) },
                  },
                ],
              },
            },
          ],
        });
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [],
          usage: {
            prompt_tokens: 2048,
            completion_tokens: 12,
            total_tokens: 2060,
            prompt_tokens_details: { cached_tokens: 2000 },
          },
        });
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "ok" } }],
        });
        const usage = {
          prompt_tokens: 2048,
          completion_tokens: 5,
          total_tokens: 2053,
        };
        if (mode === "cold") usage.prompt_tokens_details = { cached_tokens: 0 };
        if (mode === "hit") usage.prompt_tokens_details = { cached_tokens: 2000 };
        if (mode === "regress" && completions > 1) usage.prompt_tokens_details = { cached_tokens: 0 };
        // MODE=unknown leaves the usage without any cache fields.
        send({ id: `mock-${completions}`, object: "chat.completion.chunk", choices: [], usage });
      }

      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock kv-cache provider (${mode}) listening on 127.0.0.1:${port}`);
});
