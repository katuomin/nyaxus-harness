// Mock provider for the consecutive-denial circuit-breaker e2e test.
//
// Behavior: every /v1/chat/completions request streams a gated terminal call
// ("rm storm<N>.txt", unique per request) and never answers with plain text.
// The operator denies it, the daemon feeds the denial back, the mock calls the
// tool again - a denial storm. The daemon's breaker must hard-stop the run
// after 3 consecutive denials (run.failed), not grind on forever.
//
// The arguments vary per request on purpose: the repeat-tool loop guard keys
// on (tool, args) and would suppress the 3rd identical call before it reached
// the approval gate. Distinct arguments keep every call at the gate, so this
// test exercises the denial breaker in isolation.
//
// The command is a write (rm), so it is classifier-gated and really does
// prompt: read-only commands would auto-approve and there would be no denial.
//
// Usage: MOCK_PORT=8981 node harness/tests/mock_denial_storm_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8981);
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
    req.on("data", () => {});
    req.on("end", () => {
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      // The daemon drops the socket when the breaker stops the run; writing
      // to a closed socket must not crash the mock.
      res.on("error", () => {});
      req.socket.on("error", () => {});
      // A gated call, unique per request: a storm of distinct denials, not
      // progress (distinct args sidestep the repeat-tool guard, see header).
      res.write(
        `data: ${JSON.stringify({
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
                    function: { name: "terminal", arguments: JSON.stringify({ command: `rm storm${completions}.txt` }) },
                  },
                ],
              },
            },
          ],
        })}\n\n`,
      );
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock denial-storm provider listening on 127.0.0.1:${port}`);
});
