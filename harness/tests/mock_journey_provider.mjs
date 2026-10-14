// Mock historian/auxiliary provider for the memory_journey e2e test.
//
// Behavior:
//   GET  /v1/models            -> one model, so discovery succeeds
//   POST /v1/chat/completions  -> two modes, keyed on the user message:
//     * journey mode (the message contains "BEGIN NEW TURNS"): returns the
//       existing journey (if any) plus one dated segment that echoes the
//       first line of the new turns - deterministic, so the test can assert
//       on the content;
//     * anything else (initial summary / re-compression / retain): a valid
//       sectioned checkpoint, so summary-mode sessions work.
//
// Usage: MOCK_PORT=8909 node harness/tests/mock_journey_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8909);

const CHECKPOINT = [
  "## Goal",
  "journey test session.",
  "## Decisions",
  "- (none)",
  "## Constraints",
  "- (none)",
  "## Unresolved Tasks",
  "- (none)",
  "## Relevant Artifacts",
  "- (none)",
  "## Next Steps",
  "- (none)",
].join("\n");

function between(text, start, end) {
  const s = text.indexOf(start);
  if (s === -1) return "";
  const bodyStart = s + start.length;
  const e = text.indexOf(end, bodyStart);
  if (e === -1) return "";
  return text.slice(bodyStart, e).trim();
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ data: [{ id: "mock-journey", meta: { n_ctx: 16384 } }] }));
    return;
  }

  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk;
    });
    req.on("end", () => {
      let text = CHECKPOINT;
      try {
        const parsed = JSON.parse(body);
        const user = (parsed.messages || []).find((m) => m.role === "user");
        if (user && user.content && user.content.includes("BEGIN NEW TURNS")) {
          const existing = between(
            user.content,
            "BEGIN EXISTING JOURNEY\n",
            "\nEND EXISTING JOURNEY",
          );
          const turns = between(
            user.content,
            "BEGIN NEW TURNS (everything after the existing journey; each " +
              "turn is stamped [YYYY-MM-DD HH:MM UTC])\n",
            "\nEND NEW TURNS",
          );
          const firstLine =
            turns.split("\n").find((l) => l.trim().length > 0) || "";
          // The writer must stamp every turn [YYYY-MM-DD HH:MM UTC]; flag a
          // missing stamp so the test can assert on it.
          const stamped = /^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}(?: UTC)?\] /.test(
            firstLine,
          );
          const marker = firstLine
            .replace(/^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}(?: UTC)?\] /, "")
            .slice(0, 64);
          const segment =
            (stamped ? "" : "UNSTAMPED-TURN: ") +
            `2026-09-17 - journey-segment: ${marker}`;
          text =
            existing && existing !== "(empty)"
              ? `${existing}\n${segment}`
              : segment;
        }
      } catch {
        // Malformed request; keep the default checkpoint.
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      res.write(
        `data: ${JSON.stringify({
          id: "mock-journey",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: text } }],
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
  console.log(`mock journey provider listening on 127.0.0.1:${port}`);
});
