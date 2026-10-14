// Minimal OpenAI-compatible mock provider for the quiz / ask_user_question
// e2e test.
//
// Behavior (stateful, per server lifetime):
//   GET  /v1/models            -> one model, so the daemon's discovery succeeds
//   POST /v1/chat/completions  -> SSE stream
//     request 1: quiz tool call (options as a JSON array string, correct=0)
//     request 2: ask_user_question tool call
//     request 3: a plain text answer (ends the tool loop)
//
// The test answers the quiz with the presented index of the correct option
// (Paris) and the question with the first presented option, then asserts the
// graded result, the ungraded choice, and that no approval gate appeared.
//
// Usage: MOCK_PORT=9033 node harness/tests/mock_quiz_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9033);
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
        // options travel as a JSON array string (every tool argument is a
        // string); correct=0 is the original (pre-shuffle) index of Paris.
        send(toolCall("call_quiz", "quiz", {
          question: "What is the capital of France?",
          options: JSON.stringify(["Paris", "Lyon", "Marseille"]),
          correct: "0",
          explanation: "Paris is the capital of France; Lyon and Marseille are large cities, not capitals.",
        }));
      } else if (completions === 2) {
        send(toolCall("call_ask", "ask_user_question", {
          question: "Which direction should we go next?",
          options: JSON.stringify(["Deeper into the topic", "A practical example", "Something else"]),
        }));
      } else {
        send({
          id: `mock-${completions}`,
          object: "chat.completion.chunk",
          choices: [
            {
              index: 0,
              delta: { content: "quiz answer done" },
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
  console.log(`mock quiz provider listening on 127.0.0.1:${port}`);
});
