// Mock provider for the iteration-limit e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> streams a terminal tool call (echo
//     iter-N) for every request, until the request carries the daemon's
//     iteration-limit notice; that final request streams a text summary
//     reporting how many requests it took. With the daemon's 20-iteration
//     budget the summary must arrive on request 20, which proves the final
//     call was tool-less (a tool call on it would not see the notice).
//
// Usage: MOCK_PORT=8925 node harness/tests/mock_loop_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8925);
const delayMs = Number(process.env.MOCK_DELAY_MS || 100);

let requests = 0;

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
      requests += 1;
      const n = requests;
      setTimeout(() => {
        res.writeHead(200, {
          "content-type": "text/event-stream",
          "cache-control": "no-cache",
        });
        const chunk = (delta) => {
          res.write(`data: ${JSON.stringify({
            id: "mock-loop",
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta }],
          })}\n\n`);
        };
        if (body.includes("maximum number of tool-calling iterations")) {
          chunk({ content: `iteration limit summary after ${n} requests` });
        } else {
          chunk({
            tool_calls: [{
              index: 0,
              id: `call-${n}`,
              type: "function",
              // cat (not echo) with a distinct file per request: non-inert
              // and a distinct signature every time, so the loop guards
              // stay silent and the iteration budget is what gets tested.
              function: { name: "terminal", arguments: `{"command":"cat iter-${n}.txt"}` },
            }],
          });
        }
        res.write("data: [DONE]\n\n");
        res.end();
      }, delayMs);
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock loop provider listening on 127.0.0.1:${port} (delay=${delayMs}ms)`);
});
