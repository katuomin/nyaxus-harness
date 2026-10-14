// Mock Hindsight memory provider for the retain e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /version                          -> version string
//   PUT  /v1/default/banks/:bank           -> create-or-update bank (200)
//   POST /v1/default/banks/:bank/memories  -> record the retain body, ack
//   POST /v1/default/banks/:bank/memories/recall
//                                          -> tag-filtered recall over the
//                                             retained items (all_strict)
//   GET  /__retained                       -> the recorded retain bodies
//   GET  /__recalls                        -> the recorded recall bodies
//
// Usage: MOCK_PORT=8971 node harness/tests/mock_hindsight.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8971);
const retained = [];
const recalls = [];
let operations = 0;

// Explicit content-length: the daemon's HTTP client reads until EOF and
// does not decode chunked transfer encoding.
const json = (res, code, obj) => {
  const payload = JSON.stringify(obj);
  res.writeHead(code, {
    "content-type": "application/json",
    "content-length": Buffer.byteLength(payload),
  });
  res.end(payload);
};

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/version") {
    return json(res, 200, { version: "0.9.1-mock" });
  }

  const bank = req.url.match(/^\/v1\/default\/banks\/([^/]+)$/);
  if (req.method === "PUT" && bank) {
    return json(res, 200, { bank_id: decodeURIComponent(bank[1]) });
  }

  const memories = req.url.match(/^\/v1\/default\/banks\/([^/]+)\/memories$/);
  if (req.method === "POST" && memories) {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      retained.push({
        bank: decodeURIComponent(memories[1]),
        body: JSON.parse(body),
      });
      operations += 1;
      json(res, 200, { operation_id: `op-${operations}`, facts: [] });
    });
    return;
  }

  const recallRoute = req.url.match(
    /^\/v1\/default\/banks\/([^/]+)\/memories\/recall$/
  );
  if (req.method === "POST" && recallRoute) {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      const parsed = JSON.parse(body);
      const bankName = decodeURIComponent(recallRoute[1]);
      const requestedTags = Array.isArray(parsed.tags) ? parsed.tags : [];
      const results = retained
        .filter((r) => r.bank === bankName)
        .flatMap((r) => r.body.items || [])
        .filter((item) =>
          requestedTags.every((t) => (item.tags || []).includes(t))
        )
        .map((item, i) => ({
          id: `mock-${i}`,
          text: item.content,
          type: "experience",
          document_id: item.document_id,
          tags: item.tags || [],
          scores: { relevance: 1 },
        }));
      recalls.push({ bank: bankName, body: parsed });
      json(res, 200, { results, entities: [] });
    });
    return;
  }

  if (req.method === "GET" && req.url === "/__retained") {
    return json(res, 200, retained);
  }

  if (req.method === "GET" && req.url === "/__recalls") {
    return json(res, 200, recalls);
  }

  json(res, 404, { error: "not found" });
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock hindsight listening on 127.0.0.1:${port}`);
});
