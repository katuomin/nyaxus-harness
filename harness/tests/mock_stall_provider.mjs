// Mock provider for the stall-watchdog e2e test.
//
// Behavior (MODE env):
//   midstream -> every request streams one text delta ("partial "), then
//               goes silent forever: no [DONE], no close. The daemon's stall
//               watchdog must abort the stream and fail the run with the
//               partial text persisted.
//   silent    -> every request sends the SSE headers, then goes silent
//               forever (no payloads at all). The daemon must treat the stall
//               like a transport failure: run.stalled + run.retrying (the
//               single retry), a second run.stalled, then run.failed.
//
// Usage: MOCK_PORT=8977 MODE=midstream node harness/tests/mock_stall_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8977);
const mode = process.env.MODE || "midstream";
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
      // The daemon drops the socket when the watchdog fires; writing to a
      // closed socket must not crash the mock.
      res.on("error", () => {});
      req.socket.on("error", () => {});
      if (mode === "midstream") {
        res.write(
          `data: ${JSON.stringify({
            id: `mock-${completions}`,
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta: { content: "partial " } }],
          })}\n\n`,
        );
      }
      // Then silence: the response stays open (no [DONE], no end), so the
      // daemon's 2s socket polls time out until the silence cap.
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock stall provider (${mode}) listening on 127.0.0.1:${port}`);
});
