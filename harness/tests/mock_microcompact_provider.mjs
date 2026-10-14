// Mock provider for microcompact_test.sh.
//
// Request 1 emits seven terminal calls, each producing ~8 KiB of output -
// enough in-run tool results to overflow the (small) context window. Later
// requests record how many tool results arrived cleared (tier-0 marker)
// versus intact, and answer normally. A companion aux mock counts its
// requests so the test can assert the auxiliary model was never spent.
import fs from "node:fs";
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8970);
const requestLog = process.env.REQUEST_LOG || "";
const countLog = process.env.COUNT_LOG || "";
let completions = 0;

function record(body) {
  if (!requestLog) return;
  let cleared = 0;
  let intact = 0;
  try {
    const parsed = JSON.parse(body);
    for (const message of parsed.messages || []) {
      if (message.role !== "tool") continue;
      if (String(message.content || "").includes("cleared for the context budget")) cleared += 1;
      else intact += 1;
    }
  } catch {
    // The daemon's request is what this test measures; leave the record
    // useful even if a future protocol change makes it not-JSON.
  }
  fs.appendFileSync(requestLog, `${JSON.stringify({
    request_bytes: Buffer.byteLength(body, "utf8"),
    cleared,
    intact,
  })}\n`);
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Small window: the seven tool results must push the request over
    // budget so the tier-0 pass has work to do.
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 32768 } }] });
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
      if (countLog) fs.appendFileSync(countLog, "1\n");
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (delta) => res.write(`data: ${JSON.stringify({
        id: `mock-micro-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      })}\n\n`);
      if (completions === 1) {
        // Seven tool calls, each ~8.4 KiB of path-heavy output (350 lines of
        // 24 bytes). Sized so the full batch overflows the 32k window's
        // budget, but clearing the 2 oldest (tier-0 keeps 5) fits again -
        // the aux model must not be needed.
        for (let i = 0; i < 7; i += 1) {
          send({
            tool_calls: [{
              index: i,
              id: `call_${i + 1}`,
              function: {
                name: "terminal",
                arguments: JSON.stringify({
                  command: `yes C:/nyaxus/micro/00${i + 1}.txt | head -n 350`,
                }),
              },
            }],
          });
        }
      } else {
        send({ content: "microcompact kept the newest tool results" });
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
  console.log(`mock microcompact provider listening on 127.0.0.1:${port}`);
});
