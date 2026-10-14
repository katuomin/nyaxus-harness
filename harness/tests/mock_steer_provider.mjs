// Mock provider for the steer e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> request 1 streams a text fragment plus a
//     native tool call (terminal) after MOCK_FIRST_DELAY_MS; request 2 and
//     beyond stream a final text answer reporting how many user/assistant
//     messages it saw. The first-request delay opens a window in which the
//     test can queue a second message and steer the first run: the steered
//     run must end after the tool batch, before request 2.
//
// Usage: MOCK_PORT=8923 node harness/tests/mock_steer_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8923);
const firstDelayMs = Number(process.env.MOCK_FIRST_DELAY_MS || 2000);

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
      const isFirst = requests === 1;
      const delay = isFirst ? firstDelayMs : 300;
      setTimeout(() => {
        res.writeHead(200, {
          "content-type": "text/event-stream",
          "cache-control": "no-cache",
        });
        const chunk = (delta) => {
          res.write(`data: ${JSON.stringify({
            id: "mock-steer",
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta }],
          })}\n\n`);
        };
        if (isFirst) {
          // A text fragment plus one terminal tool call: the run does a tool
          // iteration, so the steer boundary is observable (request 2 never
          // happens when the steer lands in the window).
          chunk({ content: "working on it" });
          chunk({
            tool_calls: [{
              index: 0,
              id: "call-1",
              type: "function",
              function: { name: "terminal", arguments: "" },
            }],
          });
          chunk({
            tool_calls: [{
              index: 0,
              function: { arguments: '{"command":"echo tool-ran"}' },
            }],
          });
        } else {
          let text = "final: no user message";
          try {
            const messages = JSON.parse(body).messages || [];
            const users = messages.filter((m) => m.role === "user");
            const assistants = messages.filter((m) => m.role === "assistant");
            const last = users.length ? users[users.length - 1].content : "";
            text = `final: users: ${users.length}, assistants: ${assistants.length}, last: ${last}`;
          } catch {
            // Malformed request; keep the default answer.
          }
          chunk({ content: text });
        }
        res.write("data: [DONE]\n\n");
        res.end();
      }, delay);
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock steer provider listening on 127.0.0.1:${port} (first delay=${firstDelayMs}ms)`);
});
