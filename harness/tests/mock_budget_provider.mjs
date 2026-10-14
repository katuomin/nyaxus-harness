// Mock provider for context_budget_test.sh.
//
// Serves two endpoints on one server:
//   /v1/*        the primary provider. Records every chat request body and
//                enforces a serialized-size limit the way a real provider
//                enforces its token window: requests over MAX_REQUEST_BYTES
//                get an HTTP 400 exceed_context_size_error. A chat request
//                carrying the quoted-source summary prompt (the daemon's
//                primary-model fallback) answers with a valid checkpoint.
//   /aux/v1/*    a tiny auxiliary model that answers every chat request with
//                a small valid sectioned checkpoint, so the daemon's
//                compaction produces a real (small) summary instead of the
//                verbatim fallback. While the aux-fail toggle is on (set via
//                POST /control/aux-fail) it answers HTTP 500 instead.
//
// If the daemon's budget guard works, no primary request ever crosses the
// limit.
import fs from "node:fs";
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8962);
const requestLog = process.env.REQUEST_LOG || "";
const maxRequestBytes = Number(process.env.MAX_REQUEST_BYTES || 28000);
let completions = 0;
let auxFail = false;

// A minimal checkpoint that passes the daemon's sectioned-markdown
// validation: every section present, no source citations to validate.
const CHECKPOINT = [
  "## Goal",
  "Summarize the budget-marker conversation for the context budget test.",
  "## Decisions",
  "(none)",
  "## Constraints",
  "(none)",
  "## Unresolved Tasks",
  "(none)",
  "## Relevant Artifacts",
  "(none)",
  "## Next Steps",
  "(none)",
].join("\n");

function record(body, status, endpoint) {
  if (!requestLog) return;
  fs.appendFileSync(
    requestLog,
    `${JSON.stringify({
      endpoint,
      request_bytes: Buffer.byteLength(body, "utf8"),
      status,
      body,
    })}\n`
  );
}

function sse(res, id, delta) {
  res.write(
    `data: ${JSON.stringify({
      id,
      object: "chat.completion.chunk",
      choices: [{ index: 0, delta }],
    })}\n\n`
  );
}

const server = http.createServer((req, res) => {
  const url = req.url || "";

  if (req.method === "GET" && url === "/v1/models") {
    const payload = JSON.stringify({
      data: [{ id: "mock-model", meta: { n_ctx: 131072 } }],
    });
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && url === "/aux/v1/models") {
    const payload = JSON.stringify({
      data: [{ id: "mock-aux", meta: { n_ctx: 8192 } }],
    });
    res.writeHead(200, {
      "content-type": "application/json",
      "content-length": Buffer.byteLength(payload),
    });
    res.end(payload);
    return;
  }

  // Toggle the auxiliary model's health: {"fail": true} makes the aux
  // endpoint answer 500, exercising the daemon's fallback prompt.
  if (req.method === "POST" && url === "/control/aux-fail") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      try {
        auxFail = JSON.parse(body).fail === true;
      } catch {
        auxFail = false;
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ aux_fail: auxFail }));
    });
    return;
  }

  if (req.method === "POST" && url === "/aux/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      completions += 1;
      if (auxFail) {
        record(body, 500, "aux");
        res.writeHead(500, { "content-type": "application/json" });
        res.end(JSON.stringify({
          error: { code: 500, message: "auxiliary model unavailable", type: "server_error" },
        }));
        return;
      }
      record(body, 200, "aux");
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      sse(res, `mock-aux-${completions}`, { content: CHECKPOINT });
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  if (req.method === "POST" && url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => { body += chunk; });
    req.on("end", () => {
      completions += 1;
      const bytes = Buffer.byteLength(body, "utf8");
      if (bytes > maxRequestBytes) {
        record(body, 400, "primary");
        const payload = JSON.stringify({
          error: {
            code: 400,
            message: `request exceeds the available context size (request ${bytes} bytes > ${maxRequestBytes})`,
            type: "exceed_context_size_error",
          },
        });
        res.writeHead(400, { "content-type": "application/json" });
        res.end(payload);
        return;
      }
      // The daemon's primary-model summary fallback: the request carries the
      // quoted source transcript (compaction) or the pre-validated checkpoint
      // (re-compression), so answer with a valid checkpoint the way the
      // auxiliary endpoint does.
      const isSummaryCall =
        body.includes("BEGIN QUOTED SOURCE TRANSCRIPT") ||
        body.includes("BEGIN NEW CONVERSATION TURNS");
      record(body, 200, "primary");
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      sse(res, `mock-budget-${completions}`, { content: isSummaryCall ? CHECKPOINT : `budget reply ${completions}` });
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock budget provider listening on 127.0.0.1:${port}`);
});
