// Mock provider for the loop-guard e2e test.
//
// Behavior keys on the run's user message, so one mock process serves all
// four scenarios:
//   "check the build"  -> terminal {"command":"echo ok"} (inert) on every
//     request; once the request carries the inert-loop guard text, a text
//     pivot (the model answers the intervention).
//   "loop forever"     -> terminal {"command":"git status"} on every
//     request, ignoring the repeat-loop guard text (the model refuses to
//     pivot) -> the daemon's give-up narrative.
//   "steady work"      -> cycles three distinct non-inert commands, then a
//     text answer (no guard should fire).
//   "twice only"       -> terminal git status twice; once the request
//     carries the repeat note, a text answer (2nd-call reminder path).
//
// Usage: MOCK_PORT=8927 node harness/tests/mock_loop_guard_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8927);
const delayMs = Number(process.env.MOCK_DELAY_MS || 50);

const perScenario = {};

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    // Explicit content-length: the daemon's get_body client reads until EOF
    // and does not decode chunked transfer encoding.
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
      let userMsg = "";
      try {
        const messages = JSON.parse(body).messages || [];
        const lastUser = [...messages].reverse().find((m) => m.role === "user");
        userMsg = lastUser ? lastUser.content : "";
      } catch {}
      perScenario[userMsg] = (perScenario[userMsg] || 0) + 1;
      const n = perScenario[userMsg];
      setTimeout(() => {
        res.writeHead(200, {
          "content-type": "text/event-stream",
          "cache-control": "no-cache",
        });
        const chunk = (delta) => {
          res.write(`data: ${JSON.stringify({
            id: "mock-loop-guard",
            object: "chat.completion.chunk",
            choices: [{ index: 0, delta }],
          })}\n\n`);
        };
        const tool = (command) =>
          chunk({
            tool_calls: [{
              index: 0,
              id: `call-${n}`,
              type: "function",
              function: { name: "terminal", arguments: JSON.stringify({ command }) },
            }],
          });
        // Final usage chunk (OpenAI include_usage pattern: top-level usage,
        // empty choices) - the daemon's metrics.snapshot source.
        const usage = () =>
          res.write(`data: ${JSON.stringify({
            id: "mock-loop-guard",
            object: "chat.completion.chunk",
            choices: [],
            usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 },
          })}\n\n`);
        if (userMsg.includes("check the build")) {
          if (body.includes("[inert-loop guard]")) {
            chunk({ content: "pivoted to real work" });
          } else {
            tool("echo ok");
          }
        } else if (userMsg.includes("loop forever")) {
          // Ignores the guard text on purpose: the daemon must give up.
          tool("git status");
        } else if (userMsg.includes("steady work")) {
          if (n === 1) tool("git status");
          else if (n === 2) tool("ls -la");
          else if (n === 3) tool("cat file.txt");
          else chunk({ content: "steady work done" });
        } else if (userMsg.includes("twice only")) {
          if (n < 3) tool("git status");
          else if (body.includes("[repeat note]")) {
            chunk({ content: "noted the repeat" });
          } else {
            // Safety: a 3rd git status would be suppressed by the daemon;
            // answer so the scenario cannot hang.
            chunk({ content: "noted the repeat" });
          }
        } else {
          chunk({ content: "unrecognized scenario" });
        }
        usage();
        res.write("data: [DONE]\n\n");
        res.end();
      }, delayMs);
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock loop-guard provider listening on 127.0.0.1:${port} (delay=${delayMs}ms)`);
});
