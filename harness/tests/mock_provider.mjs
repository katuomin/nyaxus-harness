// Minimal OpenAI-compatible mock provider for the approval-scope e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: terminal call "rm one.txt"  (prompts; approved with session scope)
//     request 2: terminal call "rm one.txt"  (session-approved: no re-prompt)
//     request 3: terminal call "rm two.txt"  (session-approved: no re-prompt)
//     request 4: terminal call "cat big.txt" (classifier auto-approve; its
//                output exceeds the tool-output cap)
//
// The write commands (rm) are unknown to the read-only argv classifier on
// purpose: auto-approvable read-only commands would skip the gate and the
// approval-scope assertions would have nothing to approve.
//
// Tool-call arguments follow the OpenAI convention: a JSON object string
// ("{\"command\":\"...\"}"), as the daemon's dispatch requires.
//     request 5+: a plain text answer (ends the daemon's tool loop)
//
// The daemon executes one provider call per tool-loop iteration. That lets the
// test verify that "allow session" scope is a fingerprint of tool name +
// exact arguments: the same call runs without a fresh prompt, a different
// call prompts again.
//
// Usage: MOCK_PORT=8897 node harness/tests/mock_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8897);
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

      if (completions <= 4) {
        const command =
          completions === 3 ? "rm two.txt"
          : completions === 4 ? "cat big.txt"
          : "rm one.txt";
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
