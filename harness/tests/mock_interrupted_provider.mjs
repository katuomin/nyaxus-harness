// Mock provider for the interrupted-turn e2e test.
//
// Three modes (MODE env):
//   cancel   -> every request streams "partial-one ", waits INTERRUPT_DELAY_MS,
//               then streams "partial-two" and [DONE]. The test cancels the
//               run inside the gap; the first chunk is the partial response
//               that must be persisted.
//   toolfail -> request 1 streams "partial-text " plus a terminal tool call;
//               request 2+ answer 503 forever. The tool loop's second call
//               fails, so the run must end run.failed with the first
//               iteration's text persisted.
//   silent   -> every request sends the SSE headers, then goes silent for
//               SILENT_MS (a long prompt pass: no first token), then answers.
//               The test cancels during the silence; the daemon's blocked
//               socket read must notice the cancel within one 2s poll, not
//               at the end of the silence.
//
// Usage: MOCK_PORT=8930 MODE=cancel node harness/tests/mock_interrupted_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8930);
const mode = process.env.MODE || "cancel";
const delayMs = Number(process.env.INTERRUPT_DELAY_MS || 4000);
const silentMs = Number(process.env.SILENT_MS || 30000);
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
      if (mode === "toolfail" && completions >= 2) {
        // llama.cpp's busy response; keeps failing so the run cannot recover.
        const payload = JSON.stringify({ error: { message: "server is busy processing requests", type: "server_error" } });
        res.writeHead(503, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
        res.end(payload);
        return;
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      // The daemon drops the socket on cancel; writing to a closed socket
      // must not crash the mock.
      res.on("error", () => {});
      req.socket.on("error", () => {});
      const chunk = (delta) =>
        `data: ${JSON.stringify({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta }],
        })}\n\n`;
      if (mode === "toolfail") {
        res.write(chunk({ content: "partial-text " }));
        // rm (not echo): classifier-gated, so the run parks at the approval
        // gate as the test expects.
        res.write(chunk({ tool_calls: [{ index: 0, id: "call_1", function: { name: "terminal", arguments: "rm hi.txt" } }] }));
        res.write("data: [DONE]\n\n");
        res.end();
        return;
      }
      if (mode === "silent") {
        // Headers are on the wire, but no bytes until the "prompt pass" ends.
        setTimeout(() => {
          res.write(chunk({ content: "late-answer" }));
          res.write("data: [DONE]\n\n");
          res.end();
        }, silentMs);
        return;
      }
      res.write(chunk({ content: "partial-one " }));
      setTimeout(() => {
        res.write(chunk({ content: "partial-two" }));
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
  console.log(`mock interrupted provider (${mode}) listening on 127.0.0.1:${port}`);
});
