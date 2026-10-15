// Mock provider for the wave-pool e2e test: one response carrying four
// DISTINCT `sleep 1` terminal calls (read-only argv -> one Read wave; the
// distinct echoes keep the loop guard from suppressing any of them), then a
// text answer.
import http from "node:http";

const port = Number(process.env.MOCK_PORT || 8941);
const perScenario = {};

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
      let userMsg = "";
      try {
        const messages = JSON.parse(body).messages || [];
        const lastUser = [...messages].reverse().find((m) => m.role === "user");
        userMsg = lastUser ? lastUser.content : "";
      } catch {}
      perScenario[userMsg] = (perScenario[userMsg] || 0) + 1;
      const n = perScenario[userMsg];
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
      });
      const chunk = (delta) => {
        res.write(`data: ${JSON.stringify({
          id: "mock-wave-probe",
          object: "chat.completion.chunk",
          choices: [{ index: 0, delta }],
        })}\n\n`);
      };
      const tool = (index, id, command) =>
        chunk({
          tool_calls: [{
            index,
            id,
            type: "function",
            function: { name: "terminal", arguments: JSON.stringify({ command }) },
          }],
        });
      const usage = () =>
        res.write(`data: ${JSON.stringify({
          id: "mock-wave-probe",
          object: "chat.completion.chunk",
          choices: [],
          usage: { prompt_tokens: 10, completion_tokens: 5, total_tokens: 15 },
        })}\n\n`);
      if (userMsg.includes("wave probe")) {
        if (n === 1) {
          // Warmup: one serial call first, so the four concurrent spawns in
          // the next round are not all cold MSYS startups at once (a known
          // rare Windows quirk: the first spawn can hang until the daemon's
          // 60s no-output retry fires).
          tool(0, "call-w", "echo warmup");
        } else if (n === 2) {
          // Four DISTINCT commands: the loop guard suppresses repeated
          // identical calls, which would mask the wave.
          const letters = "abcd";
          for (let i = 0; i < 4; i++) tool(i, `call-${i}`, `sleep 1; echo ${letters[i]}`);
        } else {
          chunk({ content: "wave probe done" });
        }
      } else {
        chunk({ content: "unrecognized scenario" });
      }
      usage();
      res.write("data: [DONE]\n\n");
      res.end();
    });
    return;
  }

  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ error: "not found" }));
});

server.listen(port, "127.0.0.1", () => {
  console.log(`mock wave-probe provider listening on 127.0.0.1:${port}`);
});
