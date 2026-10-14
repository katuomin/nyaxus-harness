// Mock provider for the read-only argv auto-approve e2e test.
//
// Behavior keys on the run's user message, so one mock process serves all
// scenarios. The daemon prefers a unix /bin/sh (git-bash on Windows), so
// every command is a POSIX command:
//   "auto approve"    -> terminal {"command":"git status"} (read-only) ->
//     the daemon must skip the gate entirely.
//   "gate write"      -> terminal {"command":"rm x.txt"} (write) -> the
//     daemon must publish approval.requested.
//   "redirect file"   -> terminal {"command":"ls > out.txt"} -> gated
//     (output redirect to a file).
//   "redirect nul"    -> terminal {"command":"ls > /dev/null"} ->
//     auto-approved (output redirect to the null device).
//   "sequence ok"     -> terminal {"command":"ls && git status"} ->
//     auto-approved (every stage read-only).
//   "sequence mixed"  -> terminal {"command":"ls && rm x"} -> gated (one
//     write stage).
//   "dynamic gates"   -> terminal {"command":"ls $HOME"} -> gated ($
//     expansion is dynamic).
//
// Each scenario answers with a scenario-specific text on request n>=2 so the
// run cannot hang.
//
// Usage: MOCK_PORT=8929 node harness/tests/mock_argv_policy_provider.mjs
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8929);
const delayMs = Number(process.env.MOCK_DELAY_MS || 50);

const perScenario = {};

// user message -> [first tool command, final text]
const scenarios = {
  "auto approve": ["git status", "auto approve done"],
  "gate write": ["rm x.txt", "gate write done"],
  "redirect file": ["ls > out.txt", "redirect file done"],
  "redirect nul": ["ls > /dev/null", "redirect nul done"],
  "sequence ok": ["ls && git status", "sequence ok done"],
  "sequence mixed": ["ls && rm x", "sequence mixed done"],
  "dynamic gates": ["ls $HOME", "dynamic gates done"],
};

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
            id: "mock-argv-policy",
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
            id: "mock-argv-policy",
            object: "chat.completion.chunk",
            choices: [],
            usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 },
          })}\n\n`);
        let command = "";
        let finalText = "";
        for (const [key, [cmd, text]] of Object.entries(scenarios)) {
          if (userMsg.includes(key)) {
            command = cmd;
            finalText = text;
            break;
          }
        }
        if (n === 1 && command) {
          tool(command);
        } else {
          chunk({ content: finalText || "unrecognized scenario" });
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
  console.log(`mock argv-policy provider listening on 127.0.0.1:${port} (delay=${delayMs}ms)`);
});
