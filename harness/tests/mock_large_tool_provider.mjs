// Mock provider for tool_context_budget_test.sh.
//
// Request 1 emits a terminal call that produces a large path-heavy listing.
// Later requests record the serialized request and answer normally. The test
// uses this to verify that terminal trimming is a byte cap small enough to
// leave room for the provider's tokenized prompt.
import fs from "node:fs";
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8960);
const command = process.env.MOCK_COMMAND || "echo large-output";
const requestLog = process.env.REQUEST_LOG || "";
let completions = 0;

function record(body) {
  if (!requestLog) return;
  let toolBytes = 0;
  try {
    const parsed = JSON.parse(body);
    const tool = (parsed.messages || []).find((message) => message.role === "tool");
    toolBytes = tool ? Buffer.byteLength(String(tool.content || ""), "utf8") : 0;
  } catch {
    // The daemon's request is what this test measures; leave the record useful
    // even if a future protocol change makes it non-JSON.
  }
  fs.appendFileSync(requestLog, `${JSON.stringify({
    request_bytes: Buffer.byteLength(body, "utf8"),
    tool_bytes: toolBytes,
  })}\n`);
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 131072 } }] });
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      completions += 1;
      record(body);
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (delta) => res.write(`data: ${JSON.stringify({
        id: `mock-large-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      })}\n\n`);
      if (completions === 1) {
        send({
          tool_calls: [{
            index: 0,
            id: "call_large",
            function: { name: "terminal", arguments: JSON.stringify({ command }) },
          }],
        });
      } else {
        send({ content: "large output was bounded" });
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
  console.log(`mock large-tool provider listening on 127.0.0.1:${port}`);
});
