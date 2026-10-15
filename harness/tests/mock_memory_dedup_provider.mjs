// Mock memory endpoint for the memory_dedup e2e test. Serves BOTH the
// extraction endpoint and the dedup endpoint on one port; the daemon sends
// both to memory.extract_endpoint, so the mock distinguishes them by the
// system prompt.
//
// Behavior:
//   GET  /v1/models            -> one model, so discovery succeeds
//   POST /v1/chat/completions:
//     * system prompt has "deduplication judge" -> dedup mode: parse the
//       "## New candidates" JSON from the user prompt and decide per
//       candidate by position (index):
//         - index 0 -> action "skip"
//         - index 1 -> action "merge", target_ids = the candidate's related
//                      ids, merged_content = "MERGED: " + text
//         - else    -> action "store"
//     * otherwise -> extract mode: a fixed candidate list (natural text, no
//       marker tokens - markers would shift the small embedding model's
//       vectors and defeat the cosine recall). Candidate 1 is a natural
//       rephrasing of the test's pre-seeded unit (high cosine -> recalled).
//
// Usage: MOCK_PORT=9062 node harness/tests/mock_memory_dedup_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9062);

const EXTRACT_CANDIDATES = {
  candidates: [
    { type: "world", text: "The user prefers concise answers.", confidence: 0.9 },
    { type: "world", text: "The beta protocol listens on port 2222 at the gateway.", confidence: 0.9 },
    { type: "procedure", text: "The build pipeline uses the zig compiler.", confidence: 0.9 },
  ],
};

// Extract the JSON array that follows `marker` in `text`.
function extractJsonArray(text, marker) {
  const idx = text.indexOf(marker);
  if (idx === -1) return null;
  const start = text.indexOf("[", idx);
  const end = text.lastIndexOf("]");
  if (start === -1 || end === -1 || end < start) return null;
  try {
    return JSON.parse(text.slice(start, end + 1));
  } catch {
    return null;
  }
}

function decide(c, idx) {
  if (idx === 0) {
    return { id: c.id, action: "skip" };
  }
  if (idx === 1) {
    return {
      id: c.id,
      action: "merge",
      target_ids: c.related || [],
      merged_content: "MERGED: " + (c.content || ""),
      merged_type: c.type,
    };
  }
  return { id: c.id, action: "store" };
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ data: [{ id: "mock-dedup", meta: { n_ctx: 4096 } }] }));
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      let payload = {};
      try {
        payload = JSON.parse(body);
      } catch {
        payload = {};
      }
      const messages = payload.messages || [];
      const system = (messages[0] && messages[0].content) || "";
      const user = (messages[1] && messages[1].content) || "";
      let content;
      if (system.includes("deduplication judge")) {
        const cands = extractJsonArray(user, "## New candidates") || [];
        content = JSON.stringify(cands.map((c, idx) => decide(c, idx)));
      } else {
        content = JSON.stringify(EXTRACT_CANDIDATES);
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.write(
        `data: ${JSON.stringify({
          id: "mock-dedup",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content } }],
        })}\n\n`,
      );
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock dedup provider listening on 127.0.0.1:${port}`);
});
