// Minimal OpenAI-compatible mock provider for the prompt.edit e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream, scripted per request number:
//     request 1: plain text (the session pins its pack snapshot)
//     request 2: a prompt.edit tool call (the test DENIES it)
//     request 3: plain text (ends the denied run)
//     request 4: the same prompt.edit tool call (the test APPROVES it)
//     request 5: plain text (ends the approved run)
//     request 6: plain text (same session - must still see the v1 module)
//     request 7: plain text (new session - must see the v2 module)
//     request 8: a prompt.edit that DROPS the frozen section (approved;
//                the daemon's frozen-section guard must reject it)
//     request 9: plain text (ends that run)
//   GET  /stats                -> { completions, last_request }
//
// The test inspects `last_request` to verify which pack document the daemon
// assembled (the drift guard: a session keeps its pinned snapshot, new
// sessions adopt the new revision).
//
// Tool-call arguments follow the OpenAI convention: a JSON object string,
// as the daemon's dispatch requires.
//
// Usage: MOCK_PORT=9021 node harness/tests/mock_prompt_edit_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9021);
let completions = 0;
let lastRequest = "";

// The v2 edit keeps the author-declared frozen span verbatim (the
// prompt-pack module's voice anchor between `<!-- frozen -->` markers).
const V2_CORE =
  "v2 core text\n<!-- frozen -->\nPinned: I speak in short, dry sentences.\n<!-- /frozen -->";
const EDIT_ARGS = JSON.stringify({
  path: "core.md",
  content: V2_CORE,
  reason: "test edit",
});
// The v3 edit drops the frozen span: the daemon must reject it with a
// human-readable reason and leave the module untouched.
const FROZEN_EDIT_ARGS = JSON.stringify({
  path: "core.md",
  content: "v3 core text (pinned line dropped)",
  reason: "drop the pinned line",
});

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's get_body client reads until EOF
    // and does not decode chunked transfer encoding.
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }

  if (req.method === "GET" && req.url === "/stats") {
    const payload = JSON.stringify({ completions, last_request: lastRequest });
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
      lastRequest = body;
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

      if (completions === 2 || completions === 4 || completions === 8) {
        const args = completions === 8 ? FROZEN_EDIT_ARGS : EDIT_ARGS;
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: `call_${completions}`,
                    function: { name: "prompt.edit", arguments: args },
                  },
                ],
              },
            },
          ],
        });
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta: { content: "ok" } }],
        });
      }

      // Final chunk carrying usage, like llama.cpp's OpenAI server.
      send({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [],
        usage: {
          prompt_tokens: 120 + completions * 10,
          completion_tokens: 12 + completions,
          total_tokens: 132 + completions * 11,
        },
      });

      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock provider listening on 127.0.0.1:${port}`);
});
