// Minimal OpenAI-compatible mock provider for the SKILL.md e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: skill tool call (name: "test-skill")
//     request 2: skill tool call (name: "user-only-skill")
//     request 3: a plain text answer (ends the tool loop)
//
// The first request body is written to $BODY_FILE so the test can assert on
// the injected skill catalog (a stable-tier system message): the
// model-invocable skill must appear, the disable-model-invocation skill must
// not.
//
// Usage: MOCK_PORT=9031 BODY_FILE=body1.json node harness/tests/mock_skills_provider.mjs
import http from "node:http";
import { writeFileSync } from "node:fs";

const port = Number(process.env.MOCK_PORT || 9031);
const bodyFile = process.env.BODY_FILE || "";
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's client reads until EOF and does
    // not decode chunked transfer encoding on this path.
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
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
      if (completions === 1 && bodyFile) {
        writeFileSync(bodyFile, body);
      }
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const toolCall = (id, name, args) => ({
        id: `mock-${completions}`,
        object: "chat.completion.chunk",
        choices: [
          {
            index: 0,
            delta: {
              tool_calls: [
                { index: 0, id, function: { name, arguments: JSON.stringify(args) } },
              ],
            },
          },
        ],
      });

      if (completions === 1) {
        send(toolCall("call_skill", "skill", { name: "test-skill" }));
      } else if (completions === 2) {
        send(toolCall("call_skill_user", "skill", { name: "user-only-skill" }));
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: { content: "skill answer done" },
            },
          ],
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
  console.log(`mock skills provider listening on 127.0.0.1:${port}`);
});
