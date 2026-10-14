// Counting aux mock for microcompact_test.sh: records every chat completion
// request and fails (500), so a spent auxiliary model is visible in the
// count file and the daemon falls back to the deterministic document.
import fs from "node:fs";
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8971);
const countLog = process.env.COUNT_LOG || "";

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-aux", meta: { n_ctx: 32768 } }] });
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    if (countLog) fs.appendFileSync(countLog, "1\n");
    res.writeHead(500, { "content-type": "application/json" });
    res.end(JSON.stringify({ error: "aux on purpose" }));
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock aux counting listening on 127.0.0.1:${port}`);
});
