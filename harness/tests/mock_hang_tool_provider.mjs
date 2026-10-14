// Mock provider for the hung-tool delete e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> request 1: a "terminal" tool call whose
//     command sleeps ~30s (longer than the daemon's --tool-timeout-ms in the
//     test); request 2+: a plain text answer.
//
// The point is to leave a run sitting inside the terminal tool for a while,
// so the test can delete the session mid-run and assert the delete succeeds
// (cancel + bounded tool timeout) instead of blocking forever.
//
// Usage: MOCK_PORT=8917 node harness/tests/mock_hang_tool_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8917);
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
        // ~30s on Windows cmd; the test daemon caps the tool at 3s.
        send({
          id: `mock-hang-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: "call_hang",
                    function: { name: "terminal", arguments: "ping -n 31 127.0.0.1" },
                  },
                ],
              },
            },
          ],
        });
      } else {
        send({
          id: `mock-hang-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "done" } }],
        });
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
  console.log(`mock hang-tool provider listening on 127.0.0.1:${port}`);
});
