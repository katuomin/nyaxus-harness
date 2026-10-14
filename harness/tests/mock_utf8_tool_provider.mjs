// Mock provider for the tool-output UTF-8 e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> request 1: one "terminal" tool call that
//     prints a file containing invalid UTF-8 bytes (the daemon must sanitize
//     the output before the next request); request 2: a plain text answer.
//   A request body that is not valid UTF-8 is a hard 500, mirroring the real
//   provider's JSON parser (nlohmann parse_error.101 on ill-formed bytes).
//
// Usage: MOCK_PORT=8960 node harness/tests/mock_utf8_tool_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8960);
let completions = 0;

// Byte-level UTF-8 validity: Node's string decoding would silently replace
// ill-formed bytes with U+FFFD before JSON.parse ever saw them, so the check
// has to run on the raw body.
function isValidUtf8(buf) {
  let i = 0;
  while (i < buf.length) {
    const c = buf[i];
    if (c < 0x80) {
      i += 1;
      continue;
    }
    let extra;
    if ((c & 0xe0) === 0xc0) extra = 1;
    else if ((c & 0xf0) === 0xe0) extra = 2;
    else if ((c & 0xf8) === 0xf0) extra = 3;
    else return false;
    for (let k = 1; k <= extra; k += 1) {
      if (i + k >= buf.length || (buf[i + k] & 0xc0) !== 0x80) return false;
    }
    i += 1 + extra;
  }
  return true;
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      const buf = Buffer.concat(chunks);
      if (!isValidUtf8(buf)) {
        res.writeHead(500, { "content-type": "application/json" });
        res.end(JSON.stringify({
          error: { code: 500, message: "[json.exception.parse_error.101] ill-formed UTF-8 byte in request" },
        }));
        return;
      }
      completions += 1;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const chunk = (delta) => ({
        id: `mock-utf8-${completions}`,
        object: "chat.completion.chunk",
        choices: [{ index: 0, delta }],
      });

      if (completions === 1) {
        send(chunk({
          tool_calls: [
            // awk (not type/cat): classifier-gated, so the run parks at the
            // approval gate as the test expects; it prints the file verbatim.
            { index: 0, id: "call_probe", function: { name: "terminal", arguments: '{"command":"awk \'{print}\' utf8-probe.txt"}' } },
          ],
        }));
      } else {
        send(chunk({ content: "utf8 round-trip ok" }));
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
  console.log(`mock utf8-tool provider listening on 127.0.0.1:${port}`);
});
