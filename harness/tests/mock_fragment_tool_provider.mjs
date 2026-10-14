// Mock provider for the tool-streaming e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> request 1: one "terminal" tool call whose
//     arguments arrive as two SSE fragments (the parser must aggregate them
//     into a single valid call before dispatch); request 2: two tool calls
//     in one response (multi-call round-trip); request 3+: a plain text
//     answer.
//
// Usage: MOCK_PORT=8940 node harness/tests/mock_fragment_tool_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8940);
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
      const chunk = (delta) => ({
        id: `mock-frag-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });

      // Every step carries a reasoning trace so the test can assert the
      // per-step consolidation order (reasoning -> tools -> reasoning ->
      // tools -> reasoning -> text).
      if (completions === 1) {
        // One tool call, arguments split across two chunks. The daemon must
        // aggregate by index/id and never dispatch a fragment on its own.
        send(chunk({ reasoning_content: "step one thinking" }));
        send(chunk({
          tool_calls: [
            // rm (not cat/echo): classifier-gated, so each call parks at the
            // approval gate as the test asserts.
            { index: 0, id: "call_frag", function: { name: "terminal", arguments: '{"command":"rm frag' } },
          ],
        }));
        send(chunk({
          tool_calls: [
            { index: 0, function: { arguments: 'ment ok.txt"}' } },
          ],
        }));
      } else if (completions === 2) {
        // Two tool calls in a single response.
        send(chunk({ reasoning_content: "step two thinking" }));
        send(chunk({
          tool_calls: [
            { index: 0, id: "call_a", function: { name: "terminal", arguments: '{"command":"rm first-tool.txt"}' } },
            { index: 1, id: "call_b", function: { name: "terminal", arguments: '{"command":"rm second-tool.txt"}' } },
          ],
        }));
      } else {
        send(chunk({ reasoning_content: "final step thinking" }));
        send(chunk({ content: "all tools done" }));
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
  console.log(`mock fragment-tool provider listening on 127.0.0.1:${port}`);
});
