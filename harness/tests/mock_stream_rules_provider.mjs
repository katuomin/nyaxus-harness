// Mock provider for the stream-rules e2e test.
//
// Scripted per global completion count (stateful, per server lifetime):
//   request 1: text "done - all fixed." + a terminal tool call (one response
//              carrying both). The claim precedes any tool execution, so the
//              work-claim rule fires and its reminder joins the volatile
//              tail for the rest of the run.
//   request 2: echo of every system message received ("system: ..." joined
//              with "===") - lets the test assert the reminder reached the
//              provider.
//   request 3: text "ok" (ends run 1)
//   request 4: text "ok" (run 2, no claim - the rule must fire exactly once
//              across both runs)
//
// Usage: MOCK_PORT=9023 node harness/tests/mock_stream_rules_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 9023);
let completions = 0;

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
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
      res.on("error", () => {});
      req.socket.on("error", () => {});
      const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);
      const base = () => ({ id: `mock-${completions}`, object: "chat.completion.chunk", choices: [{ index: 0, delta: {} }] });

      if (completions === 1) {
        // Claim text + a real tool call in one response, so the run continues
        // and the fired reminder reaches the next provider call.
        send({ ...base(), choices: [{ index: 0, delta: { content: "done - all fixed." } }] });
        send({
          ...base(),
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: "call_1",
                    function: { name: "terminal", arguments: JSON.stringify({ command: "ls" }) },
                  },
                ],
              },
            },
          ],
        });
      } else if (completions === 2) {
        // Echo every system message the request carried.
        let text = "no system message";
        try {
          const parsed = JSON.parse(body);
          const systems = (parsed.messages || []).filter((m) => m.role === "system");
          if (systems.length) {
            text = "system: " + systems.map((m) => m.content).join("\n===\n");
          }
        } catch {
          // Malformed request; keep the default answer.
        }
        send({ ...base(), choices: [{ index: 0, delta: { content: text } }] });
      } else {
        send({ ...base(), choices: [{ index: 0, delta: { content: "ok" } }] });
      }

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
  console.log(`mock stream-rules provider listening on 127.0.0.1:${port}`);
});
